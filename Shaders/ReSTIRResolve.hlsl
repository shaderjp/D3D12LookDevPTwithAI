#if defined(VULKAN)
#define VK_BINDING(slot, descriptorSet) [[vk::binding(slot, descriptorSet)]]
#else
#define VK_BINDING(binding, set)
#endif

#ifndef D3D12LOOKDEVPT_WITH_RTXDI
#define D3D12LOOKDEVPT_WITH_RTXDI 0
#endif

#if D3D12LOOKDEVPT_WITH_RTXDI

// This bridge intentionally uses the official RTXDI reservoir implementation
// while keeping scene/light evaluation in the renderer's native ABI. A packed
// DI reservoir contains only light/sample identity, target PDF, weight sum, M,
// visibility and age -- never an RGB average.
#include "PathTracingSceneConstants.hlsli"

struct MeshVertex
{
    float3 position;
    float3 normal;
    float4 tangent;
    float2 texcoord;
};

struct RtMaterial
{
    float4 baseColorFactor;
    float4 emissiveFactor;
    uint textureBaseIndex;
    uint alphaMasked;
    float alphaCutoff;
    float normalStrength;
    float roughnessFactor;
    float metallicFactor;
    float occlusionStrength;
    uint materialFeatures;
};

static const uint MaterialFeatureBaseColorTexture = 1u << 1;
static const uint MaterialFeatureEmissiveTexture = 1u << 6;

bool HasMaterialFeature(RtMaterial material, uint feature)
{
    return (material.materialFeatures & feature) != 0u;
}

struct RtGeometryRecord
{
    uint indexOffset;
    uint indexCount;
    int baseVertex;
    uint materialIndex;
};

struct RtLight
{
    float4 positionArea;
    float4 edge0Type;
    float4 edge1;
    float4 radianceCdf;
    uint4 meshIdentity;
};

#include <Rtxdi/DI/Reservoir.hlsli>

VK_BINDING(0, 0) RaytracingAccelerationStructure g_sceneAs : register(t0, space0);
VK_BINDING(1, 0) RWTexture2D<float4> g_output : register(u0, space0);
VK_BINDING(2, 0) RWTexture2D<float4> g_accumulation : register(u1, space0);
VK_BINDING(3, 0) RWStructuredBuffer<RTXDI_PackedDIReservoir> g_restirCurrent : register(u2, space0);
VK_BINDING(4, 0) RWStructuredBuffer<RTXDI_PackedDIReservoir> g_restirHistory : register(u3, space0);
VK_BINDING(5, 0) RWStructuredBuffer<RTXDI_PackedDIReservoir> g_restirSpatial : register(u4, space0);
VK_BINDING(6, 0) RWTexture2D<float4> g_denoiseAov0 : register(u5, space0);
VK_BINDING(7, 0) RWTexture2D<float4> g_denoiseAov1 : register(u6, space0);
VK_BINDING(8, 0) RWTexture2D<float4> g_denoiseAov2 : register(u7, space0);
VK_BINDING(9, 0) RWTexture2D<float4> g_previousDenoiseAov0 : register(u11, space0);
VK_BINDING(10, 0) RWTexture2D<float4> g_previousDenoiseAov1 : register(u12, space0);
VK_BINDING(11, 0) RWTexture2D<float4> g_previousDenoiseAov2 : register(u13, space0);
VK_BINDING(12, 0) RWTexture2D<float4> g_signalCurrentRadiance : register(u17, space0);
VK_BINDING(13, 0) RWTexture2D<float4> g_signalDiffuse : register(u18, space0);
VK_BINDING(14, 0) RWTexture2D<float4> g_signalSpecular : register(u19, space0);
VK_BINDING(15, 0) RWTexture2D<float4> g_signalResidual : register(u20, space0);
VK_BINDING(16, 0) RWTexture2D<float4> g_postDenoiseHdr : register(u33, space0);
VK_BINDING(17, 0) RWTexture2D<uint> g_surfaceIdentity : register(u38, space0);
VK_BINDING(18, 0) RWTexture2D<uint> g_previousSurfaceIdentity : register(u39, space0);
VK_BINDING(19, 0) ConstantBuffer<SceneConstants> g_scene : register(b0, space0);
VK_BINDING(20, 0) StructuredBuffer<MeshVertex> g_vertices : register(t1, space0);
VK_BINDING(21, 0) StructuredBuffer<uint> g_indices : register(t2, space0);
VK_BINDING(22, 0) StructuredBuffer<RtGeometryRecord> g_geometries : register(t3, space0);
VK_BINDING(23, 0) StructuredBuffer<RtMaterial> g_materials : register(t4, space0);
VK_BINDING(24, 0) StructuredBuffer<RtLight> g_lights : register(t5, space0);
VK_BINDING(25, 0) SamplerState g_linearSampler : register(s0, space0);
#include "PathTracingQualityCounters.hlsli"
VK_BINDING(0, 1) Texture2D g_textures[] : register(t0, space1);

#define RTXDI_LIGHT_RESERVOIR_BUFFER g_restirCurrent
#include <Rtxdi/DI/ReservoirStorage.hlsli>
#include "PathTracingSampling.hlsli"

static const float PI = 3.14159265359f;
static const uint TextureSlotBaseColor = 0u;
static const uint TextureSlotEmissive = 5u;
// Path dimensions currently occupy [0, 177]. Keep the reservoir dimensions in
// a disjoint fixed range so every pixel/frame/use has a reproducible sequence.
static const uint RtxdiDimensionCandidateBase = 256u; // four dimensions per candidate
static const uint RtxdiDimensionTemporalCoin = 288u;
static const uint RtxdiDimensionSpatialRotation = 289u;
static const uint RtxdiDimensionSpatialCoinBase = 304u; // up to sixteen neighbors

struct DiSurface
{
    float3 worldPosition;
    float3 normal;
    float3 baseColor;
    float roughness;
    float metallic;
    float viewZ;
    float rayConeWidth;
    float rayConeSpread;
    uint identity;
    bool valid;
};

struct DiLightEvaluation
{
    float3 diffuse;
    float3 specular;
    float3 direction;
    float distance;
    float targetPdf;
    float sourcePdf;
    bool valid;
};

float Luminance(float3 color)
{
    return dot(color, float3(0.2126f, 0.7152f, 0.0722f));
}

uint2 RenderDimensions()
{
    return max((uint2)round(g_scene.rayOptions.zw), uint2(1u, 1u));
}

RTXDI_ReservoirBufferParameters ReservoirParameters()
{
    uint2 dimensions = RenderDimensions();
    uint blockRows = (dimensions.x + RTXDI_RESERVOIR_BLOCK_SIZE - 1u) / RTXDI_RESERVOIR_BLOCK_SIZE;
    uint blockColumns = (dimensions.y + RTXDI_RESERVOIR_BLOCK_SIZE - 1u) / RTXDI_RESERVOIR_BLOCK_SIZE;
    RTXDI_ReservoirBufferParameters parameters;
    parameters.reservoirBlockRowPitch = blockRows * RTXDI_RESERVOIR_BLOCK_SIZE * RTXDI_RESERVOIR_BLOCK_SIZE;
    parameters.reservoirArrayPitch = parameters.reservoirBlockRowPitch * blockColumns;
    parameters.pad1 = 0u;
    parameters.pad2 = 0u;
    return parameters;
}

uint ReservoirPointer(uint2 pixel)
{
    return RTXDI_ReservoirPositionToPointer(ReservoirParameters(), pixel, 0u);
}

RTXDI_DIReservoir LoadCurrentReservoir(uint2 pixel)
{
    return RTXDI_UnpackDIReservoir(g_restirCurrent[ReservoirPointer(pixel)]);
}

RTXDI_DIReservoir LoadHistoryReservoir(uint2 pixel)
{
    return RTXDI_UnpackDIReservoir(g_restirHistory[ReservoirPointer(pixel)]);
}

RTXDI_DIReservoir LoadSpatialReservoir(uint2 pixel)
{
    return RTXDI_UnpackDIReservoir(g_restirSpatial[ReservoirPointer(pixel)]);
}

void StoreCurrentReservoir(uint2 pixel, RTXDI_DIReservoir reservoir)
{
    g_restirCurrent[ReservoirPointer(pixel)] = RTXDI_PackDIReservoir(reservoir);
}

void StoreSpatialReservoir(uint2 pixel, RTXDI_DIReservoir reservoir)
{
    g_restirSpatial[ReservoirPointer(pixel)] = RTXDI_PackDIReservoir(reservoir);
}

void ClampReservoirM(inout RTXDI_DIReservoir reservoir, float maxM)
{
    maxM = max(maxM, 1.0f);
    if (reservoir.M > maxM)
    {
        // Before FinalizeResampling weightSum is the accumulated RIS weight.
        // Scale it with M so reducing history length does not brighten the
        // estimator; only the amount of reusable history is shortened.
        reservoir.weightSum *= maxM / reservoir.M;
        reservoir.M = maxM;
    }
}

float3 CameraRay(uint2 pixel, float2 jitterPixels)
{
    uint2 dimensions = RenderDimensions();
    float2 uv = (float2(pixel) + 0.5f.xx + jitterPixels) / float2(dimensions);
    float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    float4 nearPoint = mul(float4(ndc, 0.0f, 1.0f), g_scene.inverseViewProjection);
    float4 farPoint = mul(float4(ndc, 1.0f, 1.0f), g_scene.inverseViewProjection);
    nearPoint.xyz /= max(abs(nearPoint.w), 1.0e-8f);
    farPoint.xyz /= max(abs(farPoint.w), 1.0e-8f);
    return normalize(farPoint.xyz - nearPoint.xyz);
}

float3 ReconstructWorldPosition(uint2 pixel, float primaryHitT)
{
    float3 rayDirection = CameraRay(pixel, g_scene.jitterOptions.xy);
    return g_scene.cameraPosition.xyz + rayDirection * primaryHitT;
}

DiSurface LoadCurrentSurface(uint2 pixel)
{
    float4 aov0 = g_denoiseAov0[pixel];
    float4 aov1 = g_denoiseAov1[pixel];
    DiSurface surface;
    surface.viewZ = aov0.w;
    surface.normal = normalize(aov0.xyz * 2.0f - 1.0f);
    surface.baseColor = max(aov1.rgb, 0.0f.xxx);
    surface.roughness = clamp(aov1.w, 0.04f, 1.0f);
    surface.metallic = saturate(g_signalResidual[pixel].a);
    surface.identity = g_surfaceIdentity[pixel];
    float primaryHitT = g_denoiseAov2[pixel].w;
    surface.valid = primaryHitT > 0.0f && surface.viewZ > 0.0f && surface.identity != 0u;
    surface.worldPosition = surface.valid ? ReconstructWorldPosition(pixel, primaryHitT) : 0.0f.xxx;
    surface.rayConeWidth = 0.0f;
    surface.rayConeSpread = 0.0f;
    if (surface.valid)
    {
        float angularSpread = max(g_scene.performanceOptions.x, 1.0e-6f);
        surface.rayConeSpread = angularSpread;
        surface.rayConeWidth = angularSpread * primaryHitT;
    }
    return surface;
}

float3 FresnelSchlick(float cosTheta, float3 f0)
{
    return f0 + (1.0f.xxx - saturate(f0)) * pow(1.0f - saturate(cosTheta), 5.0f);
}

float DistributionGGX(float3 normal, float3 halfVector, float roughness)
{
    float alpha = max(roughness * roughness, 0.001f);
    float alpha2 = alpha * alpha;
    float nDotH = saturate(dot(normal, halfVector));
    float denominator = nDotH * nDotH * (alpha2 - 1.0f) + 1.0f;
    return alpha2 / max(PI * denominator * denominator, 1.0e-8f);
}

float SmithLambda(float nDotDirection, float roughness)
{
    float cosine = max(nDotDirection, 1.0e-6f);
    float alpha = max(roughness * roughness, 0.001f);
    float tan2 = max(1.0f - cosine * cosine, 0.0f) / (cosine * cosine);
    return 0.5f * (sqrt(1.0f + alpha * alpha * tan2) - 1.0f);
}

float SmithG1(float nDotDirection, float roughness)
{
    return nDotDirection > 0.0f ? 1.0f / (1.0f + SmithLambda(nDotDirection, roughness)) : 0.0f;
}

float MaxComponent(float3 value)
{
    return max(value.x, max(value.y, value.z));
}

float BsdfSpecularProbability(DiSurface surface, float3 viewDirection)
{
    float3 f0 = lerp(0.04f.xxx, surface.baseColor, surface.metallic);
    float fresnelWeight = saturate(MaxComponent(FresnelSchlick(saturate(dot(surface.normal, viewDirection)), f0)));
    return clamp(0.2f + 0.55f * (1.0f - surface.roughness) + 0.25f * surface.metallic + 0.25f * fresnelWeight, 0.05f, 0.95f);
}

float EvaluateBsdfPdf(DiSurface surface, float3 viewDirection, float3 lightDirection)
{
    float nDotV = saturate(dot(surface.normal, viewDirection));
    float nDotL = saturate(dot(surface.normal, lightDirection));
    float diffusePdf = nDotL / PI;
    float specularPdf = 0.0f;
    float3 halfSum = viewDirection + lightDirection;
    if (nDotV > 0.0f && nDotL > 0.0f && dot(halfSum, halfSum) > 1.0e-10f)
    {
        float3 halfVector = normalize(halfSum);
        float vDotH = abs(dot(viewDirection, halfVector));
        float lDotH = abs(dot(lightDirection, halfVector));
        float visibleNormalPdf = DistributionGGX(surface.normal, halfVector, surface.roughness)
            * SmithG1(nDotV, surface.roughness) * vDotH / max(nDotV, 1.0e-7f);
        specularPdf = visibleNormalPdf / max(4.0f * lDotH, 1.0e-7f);
    }
    float specularProbability = BsdfSpecularProbability(surface, viewDirection);
    return lerp(diffusePdf, specularPdf, specularProbability);
}

void EvaluateBsdfLobes(
    DiSurface surface,
    float3 viewDirection,
    float3 lightDirection,
    out float3 diffuse,
    out float3 specular)
{
    diffuse = 0.0f.xxx;
    specular = 0.0f.xxx;
    float nDotL = saturate(dot(surface.normal, lightDirection));
    float nDotV = saturate(dot(surface.normal, viewDirection));
    float3 halfSum = viewDirection + lightDirection;
    if (nDotL <= 0.0f || nDotV <= 0.0f || dot(halfSum, halfSum) <= 1.0e-10f)
    {
        return;
    }
    float3 halfVector = normalize(halfSum);
    float3 f0 = lerp(0.04f.xxx, surface.baseColor, surface.metallic);
    float3 fresnel = FresnelSchlick(saturate(dot(halfVector, viewDirection)), f0);
    float geometry = 1.0f / (1.0f + SmithLambda(nDotV, surface.roughness) + SmithLambda(nDotL, surface.roughness));
    specular = DistributionGGX(surface.normal, halfVector, surface.roughness) * geometry * fresnel
        / max(4.0f * nDotV * nDotL, 0.0001f) * nDotL;
    diffuse = (1.0f.xxx - fresnel) * (1.0f - surface.metallic) * surface.baseColor / PI * nDotL;
}

uint SelectLightIndex(float sample, uint lightCount)
{
    float scaledSample = min(sample, asfloat(0x3f7fffffu)) * (float)lightCount;
    uint bucket = min((uint)scaledSample, lightCount - 1u);
    float coin = frac(scaledSample);
    uint packedAlias = asuint(g_lights[bucket].edge1.w);
    float acceptProbability = (float)(packedAlias >> 16u) / 65535.0f;
    uint aliasIndex = min(packedAlias & 0xffffu, lightCount - 1u);
    return coin < acceptProbability ? bucket : aliasIndex;
}

float CandidateTriangleUvFootprint(uint3 indices, float coneWidthAtHit, float3 rayDirection);
float CandidateTextureMipLevel(uint textureIndex, float uvFootprint);

float3 EvaluateMeshEmitterRadiance(
    RtLight light,
    float3 barycentrics,
    float coneWidthAtHit,
    float3 rayDirection)
{
    if (light.meshIdentity.x == 0xffffffffu)
    {
        return 0.0f.xxx;
    }

    RtMaterial material = g_materials[light.meshIdentity.z];
    if (!HasMaterialFeature(material, MaterialFeatureEmissiveTexture))
    {
        return 0.0f.xxx;
    }

    RtGeometryRecord geometry = g_geometries[light.meshIdentity.x];
    uint first = geometry.indexOffset + light.meshIdentity.y * 3u;
    uint3 indices = uint3(g_indices[first], g_indices[first + 1u], g_indices[first + 2u]);
    float2 texcoord = g_vertices[indices.x].texcoord * barycentrics.x
        + g_vertices[indices.y].texcoord * barycentrics.y
        + g_vertices[indices.z].texcoord * barycentrics.z;
    float uvFootprint = CandidateTriangleUvFootprint(indices, coneWidthAtHit, rayDirection);
    uint textureIndex = NonUniformResourceIndex(material.textureBaseIndex + TextureSlotEmissive);
    float mipLevel = CandidateTextureMipLevel(material.textureBaseIndex + TextureSlotEmissive, uvFootprint);
    float3 emissiveTexture = g_textures[textureIndex].SampleLevel(g_linearSampler, texcoord, mipLevel).rgb;
    return emissiveTexture * material.emissiveFactor.rgb *
        material.emissiveFactor.a * g_scene.lightOptions.y;
}

DiLightEvaluation EvaluateLight(DiSurface surface, uint lightIndex, float2 uv)
{
    DiLightEvaluation result = (DiLightEvaluation)0;
    uint lightCount = (uint)round(g_scene.lightOptions.x);
    if (!surface.valid || lightIndex >= lightCount)
    {
        return result;
    }

    RtLight light = g_lights[lightIndex];
    float3 edge0 = light.edge0Type.xyz;
    float3 edge1 = light.edge1.xyz;
    float type = light.edge0Type.w;
    float3 lightPosition;
    float3 lightBarycentrics = 0.0f.xxx;
    if (type < 0.5f)
    {
        float rootU = sqrt(saturate(uv.x));
        float edgeWeight0 = rootU * (1.0f - uv.y);
        float edgeWeight1 = rootU * uv.y;
        lightBarycentrics = float3(1.0f - rootU, edgeWeight0, edgeWeight1);
        lightPosition = light.positionArea.xyz + edge0 * edgeWeight0 + edge1 * edgeWeight1;
    }
    else
    {
        lightPosition = light.positionArea.xyz + edge0 * uv.x + edge1 * uv.y;
    }

    float3 toLight = lightPosition - surface.worldPosition;
    float distanceSquared = dot(toLight, toLight);
    if (distanceSquared <= 1.0e-8f || light.positionArea.w <= 0.0f)
    {
        return result;
    }
    result.distance = sqrt(distanceSquared);
    result.direction = toLight / result.distance;
    float lightCosine = abs(dot(normalize(cross(edge0, edge1)), -result.direction));
    float selectionPdf = light.radianceCdf.w;
    if (lightCosine <= 1.0e-5f || selectionPdf <= 0.0f || dot(surface.normal, result.direction) <= 0.0f)
    {
        return result;
    }

    result.sourcePdf = selectionPdf * distanceSquared / max(light.positionArea.w * lightCosine, 1.0e-10f);
    float3 viewDirection = normalize(g_scene.cameraPosition.xyz - surface.worldPosition);
    EvaluateBsdfLobes(surface, viewDirection, result.direction, result.diffuse, result.specular);
    float3 radiance = type < 0.5f
        ? EvaluateMeshEmitterRadiance(
            light,
            lightBarycentrics,
            surface.rayConeWidth + surface.rayConeSpread * result.distance,
            result.direction)
        : light.radianceCdf.rgb * g_scene.lightOptions.z;
    // Match the path tracer's BSDF-hit technique for emissive triangles.
    float bsdfPdf = EvaluateBsdfPdf(surface, viewDirection, result.direction);
    float lightPdf2 = result.sourcePdf * result.sourcePdf;
    float misWeight = type < 0.5f ? lightPdf2 / max(lightPdf2 + bsdfPdf * bsdfPdf, 1.0e-20f) : 1.0f;
    result.diffuse *= radiance * misWeight;
    result.specular *= radiance * misWeight;
    result.targetPdf = Luminance(max(result.diffuse + result.specular, 0.0f.xxx));
    result.valid = result.sourcePdf > 0.0f && result.targetPdf > 0.0f &&
        !isnan(result.targetPdf) && !isinf(result.targetPdf);
    return result;
}

float CandidateTriangleUvFootprint(
    uint3 indices,
    float coneWidthAtHit,
    float3 rayDirection)
{
    MeshVertex v0 = g_vertices[indices.x];
    MeshVertex v1 = g_vertices[indices.y];
    MeshVertex v2 = g_vertices[indices.z];
    float3 edge0 = v1.position - v0.position;
    float3 edge1 = v2.position - v0.position;
    float2 uvEdge0 = v1.texcoord - v0.texcoord;
    float2 uvEdge1 = v2.texcoord - v0.texcoord;
    float worldArea2 = length(cross(edge0, edge1));
    float uvArea2 = abs(uvEdge0.x * uvEdge1.y - uvEdge0.y * uvEdge1.x);
    if (worldArea2 <= 1.0e-10f || uvArea2 <= 1.0e-12f)
    {
        return 0.0f;
    }
    float3 geometricNormal = normalize(cross(edge0, edge1));
    float incidence = max(abs(dot(geometricNormal, normalize(-rayDirection))), 0.15f);
    return min(max(coneWidthAtHit, 0.0f) * sqrt(uvArea2 / worldArea2) / incidence, 4.0f);
}

float CandidateTextureMipLevel(uint textureIndex, float uvFootprint)
{
    uint resourceIndex = NonUniformResourceIndex(textureIndex);
    uint width;
    uint height;
    uint mipLevels;
    g_textures[resourceIndex].GetDimensions(0u, width, height, mipLevels);
    float texelFootprint = max(max(uvFootprint, 0.0f) * (float)max(width, height), 1.0f);
    return clamp(log2(texelFootprint), 0.0f, (float)max(mipLevels, 1u) - 1.0f);
}

bool IsAlphaTransparentCandidate(
    inout RayQuery<RAY_FLAG_NONE> query,
    DiSurface surface,
    float3 rayDirection)
{
    uint geometryIndex = query.CandidateGeometryIndex();
    RtGeometryRecord geometry = g_geometries[geometryIndex];
    RtMaterial material = g_materials[geometry.materialIndex];
    if (material.alphaMasked == 0u)
    {
        return false;
    }
    if (!HasMaterialFeature(material, MaterialFeatureBaseColorTexture))
    {
        return material.baseColorFactor.a < material.alphaCutoff;
    }
    uint first = geometry.indexOffset + query.CandidatePrimitiveIndex() * 3u;
    uint3 indices = uint3(g_indices[first], g_indices[first + 1u], g_indices[first + 2u]);
    float2 bary2 = query.CandidateTriangleBarycentrics();
    float3 bary = float3(1.0f - bary2.x - bary2.y, bary2.x, bary2.y);
    float2 uv = g_vertices[indices.x].texcoord * bary.x
        + g_vertices[indices.y].texcoord * bary.y
        + g_vertices[indices.z].texcoord * bary.z;
    uint baseColorTexture = material.textureBaseIndex + TextureSlotBaseColor;
    uint textureIndex = NonUniformResourceIndex(baseColorTexture);
    float coneWidthAtHit = surface.rayConeWidth + surface.rayConeSpread * query.CandidateTriangleRayT();
    float uvFootprint = CandidateTriangleUvFootprint(indices, coneWidthAtHit, rayDirection);
    float mipLevel = CandidateTextureMipLevel(baseColorTexture, uvFootprint);
    float alpha = material.baseColorFactor.a *
        g_textures[textureIndex].SampleLevel(g_linearSampler, uv, mipLevel).a;
    // Alpha-masked base-color textures are imported with coverage-preserving
    // mips, so the authored material threshold remains invariant with LOD.
    return alpha < material.alphaCutoff;
}

float TraceVisibility(DiSurface surface, DiLightEvaluation light)
{
    RayDesc ray;
    ray.Origin = surface.worldPosition + surface.normal * g_scene.rayOptions.x;
    ray.Direction = light.direction;
    ray.TMin = g_scene.rayOptions.x;
    ray.TMax = max(light.distance - g_scene.rayOptions.x * 2.0f, ray.TMin);
    RayQuery<RAY_FLAG_NONE> query;
    query.TraceRayInline(g_sceneAs, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH, 0xff, ray);
    while (query.Proceed())
    {
        if (query.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE &&
            !IsAlphaTransparentCandidate(query, surface, light.direction))
        {
            query.CommitNonOpaqueTriangleHit();
        }
    }
    return query.CommittedStatus() == COMMITTED_NOTHING ? 1.0f : 0.0f;
}

bool ValidatePreviousSurface(uint2 currentPixel, uint2 previousPixel, float expectedPreviousDepth)
{
    float4 currentAov0 = g_denoiseAov0[currentPixel];
    float4 currentAov1 = g_denoiseAov1[currentPixel];
    float4 previousAov0 = g_previousDenoiseAov0[previousPixel];
    float4 previousAov1 = g_previousDenoiseAov1[previousPixel];
    float4 previousAov2 = g_previousDenoiseAov2[previousPixel];
    if (previousAov2.w <= 0.0f ||
        !ValidatePackedSurfaceIdentity(g_surfaceIdentity[currentPixel], g_previousSurfaceIdentity[previousPixel]))
    {
        return false;
    }
    float normalDot = dot(normalize(currentAov0.xyz * 2.0f - 1.0f), normalize(previousAov0.xyz * 2.0f - 1.0f));
    float relativeDepth = abs(previousAov0.w - expectedPreviousDepth) / max(abs(expectedPreviousDepth), 1.0f);
    float albedoDelta = length(currentAov1.rgb - previousAov1.rgb);
    float roughnessDelta = abs(currentAov1.w - previousAov1.w);
    return normalDot >= g_scene.validationOptions.x &&
        relativeDepth <= g_scene.validationOptions.y &&
        albedoDelta <= g_scene.validationOptions.z &&
        roughnessDelta <= g_scene.validationOptions.w;
}

bool ValidateSpatialSurface(uint2 centerPixel, uint2 neighborPixel)
{
    float4 center0 = g_denoiseAov0[centerPixel];
    float4 center1 = g_denoiseAov1[centerPixel];
    float4 neighbor0 = g_denoiseAov0[neighborPixel];
    float4 neighbor1 = g_denoiseAov1[neighborPixel];
    if (g_denoiseAov2[neighborPixel].w <= 0.0f)
    {
        return false;
    }
    float normalDot = dot(normalize(center0.xyz * 2.0f - 1.0f), normalize(neighbor0.xyz * 2.0f - 1.0f));
    float relativeDepth = abs(center0.w - neighbor0.w) / max(abs(center0.w), 1.0f);
    return normalDot >= g_scene.validationOptions.x &&
        relativeDepth <= g_scene.validationOptions.y &&
        length(center1.rgb - neighbor1.rgb) <= max(g_scene.validationOptions.z, 0.1f) &&
        abs(center1.w - neighbor1.w) <= max(g_scene.validationOptions.w, 0.08f);
}

float3 AcesTonemap(float3 color)
{
    const float a = 2.51f;
    const float b = 0.03f;
    const float c = 2.43f;
    const float d = 0.59f;
    const float e = 0.14f;
    return saturate((color * (a * color + b)) / (color * (c * color + d) + e));
}

float3 Tonemap(float3 color)
{
    color = max(color * exp2(g_scene.viewOptions.x), 0.0f.xxx);
    uint toneMapper = (uint)round(g_scene.viewOptions.z);
    if (toneMapper == 1u) color /= 1.0f.xxx + color;
    else if (toneMapper == 2u) color = AcesTonemap(color);
    return pow(saturate(color), 1.0f / max(g_scene.viewOptions.y, 0.01f));
}

RTXDI_DIReservoir GenerateLocalCandidates(uint2 pixel, DiSurface surface)
{
    RTXDI_DIReservoir reservoir = RTXDI_EmptyDIReservoir();
    uint lightCount = (uint)round(g_scene.lightOptions.x);
    if (surface.valid && lightCount > 0u)
    {
        uint candidateCount = clamp((uint)round(g_scene.restirDiOptions.z), 1u, 4u);
        uint sampleIndex = g_scene.frameOptions.w;
        [loop]
        for (uint candidate = 0u; candidate < candidateCount; ++candidate)
        {
            uint dimensionBase = RtxdiDimensionCandidateBase + candidate * 4u;
            uint lightIndex = SelectLightIndex(
                OwenScrambledSobol1D(pixel, sampleIndex, dimensionBase),
                lightCount);
            float2 uv = OwenScrambledSobol2D(pixel, sampleIndex, dimensionBase + 1u);
            DiLightEvaluation evaluation = EvaluateLight(surface, lightIndex, uv);
            if (evaluation.valid)
            {
                RTXDI_StreamSample(
                    reservoir,
                    lightIndex,
                    uv,
                    OwenScrambledSobol1D(pixel, sampleIndex, dimensionBase + 3u),
                    evaluation.targetPdf,
                    rcp(evaluation.sourcePdf));
            }
            else
            {
                reservoir.M += 1.0f;
            }
        }
        RTXDI_FinalizeResampling(reservoir, 1.0f, max(reservoir.M, 1.0f));
    }
    return reservoir;
}

RTXDI_DIReservoir ApplyTemporalReuse(
    uint2 pixel,
    DiSurface surface,
    RTXDI_DIReservoir current)
{
    uint2 dimensions = RenderDimensions();
    RTXDI_DIReservoir result = RTXDI_EmptyDIReservoir();
    RTXDI_CombineDIReservoirs(result, current, 0.5f, current.targetPdf);
    bool acceptedHistory = false;
    uint acceptedHistoryAge = 0u;

    uint historyDomains = (uint)round(g_scene.environmentOptions.w);
    bool useHistory = surface.valid && g_scene.restirDiOptions.x > 0.5f &&
        g_scene.restirStabilityOptions.x > 0.5f &&
        (historyDomains & (HISTORY_DOMAIN_SURFACE | HISTORY_DOMAIN_LIGHTING)) ==
            (HISTORY_DOMAIN_SURFACE | HISTORY_DOMAIN_LIGHTING);
    if (useHistory)
    {
        float4 motion = g_denoiseAov2[pixel];
        float2 jitterDelta = g_scene.jitterOptions.xy - g_scene.jitterOptions.zw;
        float2 historyPosition = float2(pixel) + motion.xy * float2(dimensions) + jitterDelta;
        int2 basePixel = int2(floor(historyPosition));
        float2 fraction = frac(historyPosition);
        float bestWeight = -1.0f;
        uint2 bestPixel = 0u.xx;
        bool found = false;
        [unroll]
        for (int y = 0; y < 2; ++y)
        {
            [unroll]
            for (int x = 0; x < 2; ++x)
            {
                int2 candidate = basePixel + int2(x, y);
                if (any(candidate < 0) || any(candidate >= int2(dimensions))) continue;
                uint2 candidatePixel = uint2(candidate);
                float weight = (x == 0 ? 1.0f - fraction.x : fraction.x)
                    * (y == 0 ? 1.0f - fraction.y : fraction.y);
                if (weight > bestWeight && ValidatePreviousSurface(pixel, candidatePixel, surface.viewZ + motion.z))
                {
                    bestWeight = weight;
                    bestPixel = candidatePixel;
                    found = true;
                }
            }
        }

        // Reservoirs carry a discrete light identity, so the valid bilinear
        // taps cannot be blended. If all four taps are rejected, perform the
        // same depth-aware one-pixel dilation used by the surface-history
        // contract and select the closest valid depth instead.
        if (!found)
        {
            int2 dilationCenter = int2(round(historyPosition));
            float bestDepthError = 1.0e30f;
            [unroll]
            for (int y = -1; y <= 1; ++y)
            {
                [unroll]
                for (int x = -1; x <= 1; ++x)
                {
                    int2 candidate = dilationCenter + int2(x, y);
                    if (any(candidate < 0) || any(candidate >= int2(dimensions))) continue;
                    uint2 candidatePixel = uint2(candidate);
                    if (!ValidatePreviousSurface(pixel, candidatePixel, surface.viewZ + motion.z)) continue;
                    float depthError = abs(g_previousDenoiseAov0[candidatePixel].w - (surface.viewZ + motion.z));
                    if (depthError < bestDepthError)
                    {
                        bestDepthError = depthError;
                        bestPixel = candidatePixel;
                        found = true;
                    }
                }
            }
        }

        if (found)
        {
            RTXDI_DIReservoir previous = LoadHistoryReservoir(bestPixel);
            previous.M = min(previous.M, max(g_scene.restirDiOptions.w, 1.0f));
            previous.age = min(previous.age + 1u, 255u);
            if (previous.age > (uint)max(round(g_scene.restirStabilityOptions.w), 1.0f))
            {
                previous = RTXDI_EmptyDIReservoir();
            }
            if (RTXDI_IsValidDIReservoir(previous))
            {
                uint lightIndex = RTXDI_GetDIReservoirLightIndex(previous);
                DiLightEvaluation evaluation = EvaluateLight(surface, lightIndex, RTXDI_GetDIReservoirSampleUV(previous));
                float targetAtCurrent = evaluation.valid ? evaluation.targetPdf : 0.0f;
                float selectionCoin = OwenScrambledSobol1D(
                    pixel,
                    g_scene.frameOptions.w,
                    RtxdiDimensionTemporalCoin);
                RTXDI_CombineDIReservoirs(result, previous, selectionCoin, targetAtCurrent);
                acceptedHistory = targetAtCurrent > 0.0f;
                acceptedHistoryAge = previous.age;
            }
        }
    }
    // RTXDI's age field normally describes cached visibility. This renderer
    // always traces final visibility, so use it as the surface-reservoir
    // history length instead. Pass B uses it to spend eight neighbors only on
    // disocclusions and young history, and four on stable surfaces.
    result.age = acceptedHistory ? acceptedHistoryAge : 0u;
    ClampReservoirM(result, g_scene.restirDiOptions.w);
    RTXDI_FinalizeResampling(result, 1.0f, max(result.M, 1.0f));
    return result;
}

// Pass A: candidate generation and temporal resampling are register-local.
// The previous-frame reservoir (g_restirHistory) remains immutable for the
// complete dispatch; only the A/B scratch reservoir is written.
[numthreads(8, 8, 1)]
void RtxdiDiCandidateCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint2 pixel = dispatchThreadId.xy;
    uint2 dimensions = RenderDimensions();
    if (any(pixel >= dimensions)) return;

    DiSurface surface = LoadCurrentSurface(pixel);
    RTXDI_DIReservoir local = GenerateLocalCandidates(pixel, surface);
    StoreSpatialReservoir(pixel, ApplyTemporalReuse(pixel, surface, local));
}

// Kept as a shader-build compatibility entry point. Runtime dispatch uses the
// fused candidate/temporal pass above.
[numthreads(8, 8, 1)]
void RtxdiDiTemporalCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
}

static const int2 SpatialOffsets[16] =
{
    int2(-1, 0), int2(1, 0), int2(0, -1), int2(0, 1),
    int2(-1, -1), int2(1, -1), int2(-1, 1), int2(1, 1),
    int2(-2, 0), int2(2, 0), int2(0, -2), int2(0, 2),
    int2(-2, -1), int2(2, 1), int2(-1, 2), int2(1, -2)
};

void ShadeAndPublishReservoir(
    uint2 pixel,
    uint2 dimensions,
    DiSurface surface,
    inout RTXDI_DIReservoir reservoir)
{
    float3 diffuse = 0.0f.xxx;
    float3 specular = 0.0f.xxx;
    float inputContributionEnergy = 0.0f;
    float outputContributionEnergy = 0.0f;
    if (surface.valid && RTXDI_IsValidDIReservoir(reservoir))
    {
        DiLightEvaluation evaluation = EvaluateLight(
            surface,
            RTXDI_GetDIReservoirLightIndex(reservoir),
            RTXDI_GetDIReservoirSampleUV(reservoir));
        // Re-evaluate target/PDF and final visibility at the current surface.
        // Invalid/stale samples are removed before direct history publication.
        if (evaluation.valid)
        {
            reservoir.targetPdf = evaluation.targetPdf;
            float visibility = TraceVisibility(surface, evaluation);
            uint reservoirHistoryAge = reservoir.age;
            RTXDI_StoreVisibilityInDIReservoir(reservoir, visibility.xxx, visibility <= 0.0f);
            // Preserve the temporal surface-history length. Visibility is not
            // reused by this path; it is re-evaluated exactly every frame.
            reservoir.age = reservoirHistoryAge;
            float invPdf = RTXDI_GetDIReservoirInvPdf(reservoir);
            diffuse = evaluation.diffuse * invPdf * visibility;
            specular = evaluation.specular * invPdf * visibility;
            float contributionLimit = max(g_scene.giOptions.y, 0.0f);
            float luminance = Luminance(diffuse + specular);
            inputContributionEnergy = luminance;
            if (contributionLimit > 0.0f && luminance > contributionLimit)
            {
                float compression = contributionLimit / max(luminance, 1.0e-6f);
                diffuse *= compression;
                specular *= compression;
            }
            outputContributionEnergy = Luminance(diffuse + specular);
        }
        else
        {
            reservoir = RTXDI_EmptyDIReservoir();
        }
    }
    StoreCurrentReservoir(pixel, reservoir);

    float4 diffuseSignal = g_signalDiffuse[pixel];
    float4 specularSignal = g_signalSpecular[pixel];
    diffuseSignal.rgb += diffuse;
    specularSignal.rgb += specular;
    g_signalDiffuse[pixel] = diffuseSignal;
    g_signalSpecular[pixel] = specularSignal;

    if (QualityContributionEnabled(dimensions))
    {
        float2 pathEnergy = UnpackQualityContributionEnergy(g_qualityContribution[pixel]);
        g_qualityContribution[pixel] = PackQualityContributionEnergy(
            pathEnergy.x + inputContributionEnergy,
            pathEnergy.y + outputContributionEnergy);
    }

    // A denoiser/composite graph reconstructs beauty directly from the split
    // signals below. Avoid writing and tonemapping three full-resolution
    // intermediates that the graph immediately overwrites.
    if ((uint)round(g_scene.debugOptions.x) == 0u && g_scene.performanceOptions.w < 0.5f)
    {
        float3 hdr = max(g_postDenoiseHdr[pixel].rgb + diffuse + specular, 0.0f.xxx);
        g_postDenoiseHdr[pixel] = float4(hdr, 1.0f);
        g_accumulation[pixel] = float4(hdr, 1.0f);
        g_output[pixel] = float4(Tonemap(hdr), 1.0f);
    }
}

// Pass B: spatial reuse reads only immutable Pass-A scratch. It then performs
// exact target/PDF/visibility evaluation, shades, and publishes the next-frame
// history directly into the other physical reservoir.
[numthreads(8, 8, 1)]
void RtxdiDiSpatialCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint2 pixel = dispatchThreadId.xy;
    uint2 dimensions = RenderDimensions();
    if (any(pixel >= dimensions)) return;

    DiSurface surface = LoadCurrentSurface(pixel);
    RTXDI_DIReservoir center = LoadSpatialReservoir(pixel);
    RTXDI_DIReservoir result = RTXDI_EmptyDIReservoir();
    RTXDI_CombineDIReservoirs(result, center, 0.5f, center.targetPdf);
    uint spatialPasses = clamp((uint)round(g_scene.restirDiOptions.y), 0u, 4u);
    if (surface.valid && spatialPasses > 0u)
    {
        // The default two-pass setting maps to four samples on stable history
        // and eight for disocclusion/young history. Higher legacy settings do
        // not silently inflate this optimized interactive kernel to 16 taps.
        uint configuredSamples = min(spatialPasses * 4u, 8u);
        uint sampleCount = min(center.age <= 1u ? 8u : 4u, configuredSamples);
        float radiusScale = max(g_scene.restirOptions.z / 16.0f, 0.25f);
        uint sampleIndex = g_scene.frameOptions.w;
        uint offsetRotation = uint(OwenScrambledSobol1D(
            pixel,
            sampleIndex,
            RtxdiDimensionSpatialRotation) * 16.0f) & 15u;
        [loop]
        for (uint i = 0u; i < sampleCount; ++i)
        {
            int2 offset = int2(round(float2(SpatialOffsets[(i + offsetRotation) & 15u]) * radiusScale));
            if (all(offset == 0)) offset.x = 1;
            int2 neighbor = int2(pixel) + offset;
            if (any(neighbor < 0) || any(neighbor >= int2(dimensions))) continue;
            uint2 neighborPixel = uint2(neighbor);
            if (!ValidateSpatialSurface(pixel, neighborPixel)) continue;

            RTXDI_DIReservoir candidate = LoadSpatialReservoir(neighborPixel);
            candidate.M = min(candidate.M, max(g_scene.restirDiOptions.w, 1.0f));
            candidate.spatialDistance += offset;
            if (!RTXDI_IsValidDIReservoir(candidate)) continue;
            DiLightEvaluation evaluation = EvaluateLight(
                surface,
                RTXDI_GetDIReservoirLightIndex(candidate),
                RTXDI_GetDIReservoirSampleUV(candidate));
            float selectionCoin = OwenScrambledSobol1D(
                pixel,
                sampleIndex,
                RtxdiDimensionSpatialCoinBase + i);
            RTXDI_CombineDIReservoirs(result, candidate, selectionCoin, evaluation.valid ? evaluation.targetPdf : 0.0f);
        }
    }
    ClampReservoirM(result, g_scene.restirDiOptions.w);
    RTXDI_FinalizeResampling(result, 1.0f, max(result.M, 1.0f));
    ShadeAndPublishReservoir(pixel, dimensions, surface, result);
}

// Kept as a shader-build compatibility entry point. Runtime dispatch uses the
// fused spatial/visibility/shade pass above.
[numthreads(8, 8, 1)]
void RtxdiDiShadeCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
}

#else

// Keep every build configuration shader-complete. These entry points are not
// dispatched when RTXDI is disabled or unavailable.
[numthreads(8, 8, 1)] void RtxdiDiCandidateCS(uint3 dispatchThreadId : SV_DispatchThreadID) { }
[numthreads(8, 8, 1)] void RtxdiDiTemporalCS(uint3 dispatchThreadId : SV_DispatchThreadID) { }
[numthreads(8, 8, 1)] void RtxdiDiSpatialCS(uint3 dispatchThreadId : SV_DispatchThreadID) { }
[numthreads(8, 8, 1)] void RtxdiDiShadeCS(uint3 dispatchThreadId : SV_DispatchThreadID) { }

#endif
