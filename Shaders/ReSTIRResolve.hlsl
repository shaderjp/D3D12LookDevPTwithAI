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
    float transmissionFactor;
    float indexOfRefraction;
    uint thinDielectric;
    uint materialPadding;
    float4 uvScaleOffset;
};

static const uint MaterialFeatureBaseColorTexture = 1u << 1;
static const uint MaterialFeatureNormalTexture = 1u << 2;
static const uint MaterialFeatureRoughnessTexture = 1u << 3;
static const uint MaterialFeatureMetallicTexture = 1u << 4;
static const uint MaterialFeatureOcclusionTexture = 1u << 5;
static const uint MaterialFeatureEmissiveTexture = 1u << 6;
static const uint MaterialFeatureAlphaTexture = 1u << 7;

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

struct RtInstance
{
    float4 objectToWorldColumn0;
    float4 objectToWorldColumn1;
    float4 objectToWorldColumn2;
    float4 objectToWorldColumn3;
    float4 normalToWorldColumn0;
    float4 normalToWorldColumn1;
    float4 normalToWorldColumn2;
    float4 normalToWorldColumn3;
};

#include <Rtxdi/DI/Reservoir.hlsli>
#include <Rtxdi/GI/ReSTIRGIParameters.h>

// ReSTIRPTParameters.h deliberately exposes 16-bit C++ enum storage that is
// not accepted by every SM 6.6 compiler configuration. The shader only needs
// the SDK's fixed 64-byte packed reservoir contract.
struct RTXDI_PackedPTReservoir
{
    uint4 Data0;
    uint4 Data1;
    uint4 Data2;
    uint4 Data3;
};

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
VK_BINDING(26, 0) RWStructuredBuffer<RTXDI_PackedGIReservoir> g_restirGiCurrent : register(u43, space0);
VK_BINDING(27, 0) RWStructuredBuffer<RTXDI_PackedGIReservoir> g_restirGiHistory : register(u44, space0);
VK_BINDING(28, 0) RWStructuredBuffer<RTXDI_PackedPTReservoir> g_restirPtCurrent : register(u45, space0);
VK_BINDING(29, 0) RWStructuredBuffer<RTXDI_PackedPTReservoir> g_restirPtHistory : register(u46, space0);
VK_BINDING(19, 0) ConstantBuffer<SceneConstants> g_scene : register(b0, space0);
VK_BINDING(20, 0) StructuredBuffer<MeshVertex> g_vertices : register(t1, space0);
VK_BINDING(21, 0) StructuredBuffer<uint> g_indices : register(t2, space0);
VK_BINDING(22, 0) StructuredBuffer<RtGeometryRecord> g_geometries : register(t3, space0);
VK_BINDING(23, 0) StructuredBuffer<RtMaterial> g_materials : register(t4, space0);
VK_BINDING(24, 0) StructuredBuffer<RtLight> g_lights : register(t5, space0);
VK_BINDING(30, 0) StructuredBuffer<RtInstance> g_instances : register(t6, space0);
VK_BINDING(31, 0) RWTexture2D<float4> g_primaryPositionCone : register(u53, space0);
VK_BINDING(32, 0) RWTexture2D<float4> g_primaryGeometricNormal : register(u54, space0);
VK_BINDING(25, 0) SamplerState g_linearSampler : register(s0, space0);
#include "PathTracingQualityCounters.hlsli"
VK_BINDING(0, 1) Texture2D g_textures[] : register(t0, space1);

#define RTXDI_LIGHT_RESERVOIR_BUFFER g_restirCurrent
#include <Rtxdi/DI/ReservoirStorage.hlsli>
#define RTXDI_GI_RESERVOIR_BUFFER g_restirGiCurrent
#include <Rtxdi/GI/Reservoir.hlsli>
#include <Rtxdi/Utils/Math.hlsli>
#define RTXDI_PT_RESERVOIR_BUFFER g_restirPtCurrent
#include <Rtxdi/PT/Reservoir.hlsli>
#include "PathTracingSampling.hlsli"

static const float PI = 3.14159265359f;
static const uint TextureSlotBaseColor = 0u;
static const uint TextureSlotEmissive = 5u;
static const uint TextureSlotAlpha = 6u;
// Path dimensions currently occupy [0, 177]. Keep the reservoir dimensions in
// a disjoint fixed range so every pixel/frame/use has a reproducible sequence.
static const uint RtxdiDimensionCandidateBase = 256u; // four dimensions per candidate
static const uint RtxdiDimensionTemporalCoin = 288u;
static const uint RtxdiDimensionSpatialRotation = 289u;
static const uint RtxdiDimensionSpatialCoinBase = 304u; // up to sixteen neighbors
static const uint RtxdiSunLightIndex = 0x7ffffffdu;
static const uint RtxdiEnvironmentLightIndex = 0x7ffffffeu;

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
    float3 viewDirection;
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
    // Probability of selecting the retained light identity. This is the
    // measure stored by the DI reservoir and must not contain a
    // receiver-dependent area-to-solid-angle Jacobian.
    float sourcePdf;
    // Conditional PDF of the reconstructed point/direction for that identity,
    // measured in solid angle at the receiving surface.
    float solidAnglePdf;
    bool valid;
};

float Luminance(float3 color)
{
    return dot(color, float3(0.2126f, 0.7152f, 0.0722f));
}

void AddSignalContribution(
    inout float4 signal,
    float3 contribution,
    float hitDistance)
{
    contribution = max(contribution, 0.0f.xxx);
    float contributionLuminance = Luminance(contribution);
    float previousLuminance = Luminance(max(signal.rgb, 0.0f.xxx));
    if (hitDistance > 0.0f && contributionLuminance > 0.0f)
    {
        if (signal.a <= 0.0f || previousLuminance <= 0.0f)
        {
            signal.a = hitDistance;
        }
        else
        {
            // Direct, finite-GI and environment estimators share one NRD
            // signal. A winner-takes-all distance made alpha jump between a
            // local secondary hit and rayTMax, splitting REBLUR into a visible
            // salt-and-pepper mask. A luminance-weighted log mean remains
            // positive, treats distance ratios symmetrically, and changes
            // continuously as additive estimator energy changes.
            float contributionFraction = contributionLuminance /
                max(previousLuminance + contributionLuminance, 1.0e-8f);
            signal.a = exp2(lerp(
                log2(max(signal.a, 1.0e-6f)),
                log2(max(hitDistance, 1.0e-6f)),
                contributionFraction));
        }
    }
    signal.rgb += contribution;
}

void UnifiedLightProbabilities(
    out float areaProbability,
    out float sunProbability,
    out float environmentProbability)
{
    float areaWeight = max(g_scene.unifiedLightOptions.x, 0.0f);
    float sunWeight = g_scene.debugOptions.z > 0.5f
        ? max(Luminance(max(g_scene.lightColor.rgb * g_scene.lightColor.a, 0.0f.xxx)), 0.0f)
        : 0.0f;
    float environmentWeight = 0.0f;
    if (g_scene.debugOptions.w > 0.5f)
    {
        environmentWeight = g_scene.environmentOptions.x > 0.5f
            ? max(Luminance(max(g_scene.environmentTint.rgb * g_scene.environmentOptions.y, 0.0f.xxx)), 0.0f) * (4.0f * PI)
            : max(Luminance(max(g_scene.skyColor.rgb * g_scene.skyColor.a, 0.0f.xxx)), 0.0f) * (4.0f * PI);
    }
    float totalWeight = areaWeight + sunWeight + environmentWeight;
    if (totalWeight <= 1.0e-8f)
    {
        areaProbability = 0.0f;
        sunProbability = 0.0f;
        environmentProbability = 0.0f;
        return;
    }
    float activeFamilyCount =
        (areaWeight > 0.0f ? 1.0f : 0.0f) +
        (sunWeight > 0.0f ? 1.0f : 0.0f) +
        (environmentWeight > 0.0f ? 1.0f : 0.0f);
    float uniformActiveProbability = rcp(max(activeFamilyCount, 1.0f));
    float defensiveFraction = saturate(g_scene.unifiedLightOptions.y);
    float powerFraction = 1.0f - defensiveFraction;
    areaProbability =
        powerFraction * areaWeight / totalWeight +
        defensiveFraction * (areaWeight > 0.0f ? uniformActiveProbability : 0.0f);
    sunProbability =
        powerFraction * sunWeight / totalWeight +
        defensiveFraction * (sunWeight > 0.0f ? uniformActiveProbability : 0.0f);
    environmentProbability =
        powerFraction * environmentWeight / totalWeight +
        defensiveFraction * (environmentWeight > 0.0f ? uniformActiveProbability : 0.0f);
}

float3 RotateEnvironmentYaw(float3 direction, float angle)
{
    float sine;
    float cosine;
    sincos(angle, sine, cosine);
    return float3(
        cosine * direction.x + sine * direction.z,
        direction.y,
        cosine * direction.z - sine * direction.x);
}

float3 WorldToEnvironmentDirection(float3 direction)
{
    float3 lightDirection = normalize(mul(float4(direction, 0.0f), g_scene.environmentWorldToLight).xyz);
    return RotateEnvironmentYaw(lightDirection, g_scene.environmentOptions.z);
}

float3 EnvironmentToWorldDirection(float3 direction)
{
    float3 lightDirection = RotateEnvironmentYaw(direction, -g_scene.environmentOptions.z);
    return normalize(mul(float4(lightDirection, 0.0f), g_scene.environmentLightToWorld).xyz);
}

float CopySign(float magnitude, float signSource)
{
    return signSource < 0.0f ? -abs(magnitude) : abs(magnitude);
}

float3 EqualAreaSquareToSphere(float2 p)
{
    float u = 2.0f * p.x - 1.0f;
    float v = 2.0f * p.y - 1.0f;
    float up = abs(u);
    float vp = abs(v);
    float signedDistance = 1.0f - (up + vp);
    float r = 1.0f - abs(signedDistance);
    float phi = (r == 0.0f ? 1.0f : (vp - up) / r + 1.0f) * PI * 0.25f;
    float z = CopySign(1.0f - r * r, signedDistance);
    float cosPhi = CopySign(cos(phi), u);
    float sinPhi = CopySign(sin(phi), v);
    float diskScale = r * sqrt(max(2.0f - r * r, 0.0f));
    return float3(cosPhi * diskScale, sinPhi * diskScale, z);
}

float2 EqualAreaSphereToSquare(float3 direction)
{
    float3 d = normalize(direction);
    float x = abs(d.x);
    float y = abs(d.y);
    float r = sqrt(max(1.0f - abs(d.z), 0.0f));
    float a = max(x, y);
    float b = a == 0.0f ? 0.0f : min(x, y) / a;
    float phi = atan(b) * (2.0f / PI);
    if (x < y) phi = 1.0f - phi;
    float v = phi * r;
    float u = r - v;
    if (d.z < 0.0f)
    {
        float oldU = u;
        u = 1.0f - v;
        v = 1.0f - oldU;
    }
    u = CopySign(u, d.x);
    v = CopySign(v, d.y);
    return 0.5f * (float2(u, v) + 1.0f);
}

float2 EnvironmentUv(float3 direction)
{
    direction = WorldToEnvironmentDirection(direction);
    if (g_scene.environmentOptions.x > 1.5f)
    {
        return EqualAreaSphereToSquare(direction);
    }
    float phi = atan2(direction.x, direction.z);
    float u = frac(phi / (2.0f * PI) + 0.5f);
    float v = acos(clamp(direction.y, -1.0f, 1.0f)) / PI;
    return float2(u, v);
}

float EnvironmentTexelSolidAngle(uint row, uint width, uint height)
{
    if (g_scene.environmentOptions.x > 1.5f)
    {
        return (4.0f * PI) / max((float)(width * height), 1.0f);
    }
    float theta0 = PI * (float)row / (float)height;
    float theta1 = PI * (float)(row + 1u) / (float)height;
    return (2.0f * PI / (float)width) * max(cos(theta0) - cos(theta1), 1.0e-10f);
}

float EnvironmentDirectionPdf(float3 direction)
{
    uint environmentTextureIndex = (uint)round(g_scene.lightOptions.w);
    uint aliasTextureIndex = NonUniformResourceIndex(environmentTextureIndex + 1u);
    uint width;
    uint height;
    g_textures[aliasTextureIndex].GetDimensions(width, height);
    float2 uv = EnvironmentUv(direction);
    uint2 texel = min(
        (uint2)(uv * float2(width, height)),
        uint2(width - 1u, height - 1u));
    float texelProbability =
        g_textures[aliasTextureIndex].Load(int3(texel, 0)).z;
    return texelProbability /
        EnvironmentTexelSolidAngle(texel.y, width, height);
}

void SampleEnvironmentDirection(float2 randomUv, out float3 direction, out float directionPdf)
{
    // The alias selector is transient sampling state, not a reconstructible
    // light sample. Generate a direction here; the reservoir stores its
    // lat-long coordinates instead.
    float2 intraTexel = frac(float2(
        sin(dot(randomUv, float2(12.9898f, 78.233f))),
        sin(dot(randomUv, float2(39.3468f, 11.135f)))) * 43758.5453f);
    uint environmentTextureIndex = (uint)round(g_scene.lightOptions.w);
    uint aliasTextureIndex = NonUniformResourceIndex(environmentTextureIndex + 1u);
    uint width;
    uint height;
    g_textures[aliasTextureIndex].GetDimensions(width, height);
    uint entryCount = max(width * height, 1u);
    uint bucket = min((uint)(randomUv.x * (float)entryCount), entryCount - 1u);
    uint2 bucketTexel = uint2(bucket % width, bucket / width);
    float4 aliasEntry = g_textures[aliasTextureIndex].Load(int3(bucketTexel, 0));
    uint selected = randomUv.y < saturate(aliasEntry.x)
        ? bucket
        : min((uint)round(aliasEntry.y), entryCount - 1u);
    uint2 selectedTexel = uint2(selected % width, selected / width);
    float u = ((float)selectedTexel.x + intraTexel.x) / (float)width;
    float v = ((float)selectedTexel.y + intraTexel.y) / (float)height;
    if (g_scene.environmentOptions.x > 1.5f)
    {
        direction = EnvironmentToWorldDirection(EqualAreaSquareToSphere(float2(u, v)));
        float texelProbability = g_textures[aliasTextureIndex].Load(int3(selectedTexel, 0)).z;
        directionPdf = texelProbability / EnvironmentTexelSolidAngle(selectedTexel.y, width, height);
        return;
    }
    float theta0 = PI * (float)selectedTexel.y / (float)height;
    float theta1 = PI * (float)(selectedTexel.y + 1u) / (float)height;
    float cosTheta = lerp(cos(theta0), cos(theta1), intraTexel.y);
    float sinTheta = sqrt(saturate(1.0f - cosTheta * cosTheta));
    float phi = 2.0f * PI * (u - 0.5f);
    direction = EnvironmentToWorldDirection(float3(sinTheta * sin(phi), cosTheta, sinTheta * cos(phi)));
    float texelProbability = g_textures[aliasTextureIndex].Load(int3(selectedTexel, 0)).z;
    directionPdf = texelProbability / EnvironmentTexelSolidAngle(selectedTexel.y, width, height);
}

float3 DecodeEnvironmentDirection(float2 uv)
{
    if (g_scene.environmentOptions.x > 1.5f)
    {
        return EnvironmentToWorldDirection(EqualAreaSquareToSphere(uv));
    }
    float theta = PI * saturate(uv.y);
    float phi = 2.0f * PI * (frac(uv.x) - 0.5f);
    float sinTheta = sin(theta);
    return EnvironmentToWorldDirection(float3(
        sinTheta * sin(phi),
        cos(theta),
        sinTheta * cos(phi)));
}

float3 EvaluateEnvironment(float3 direction)
{
    if (g_scene.environmentOptions.x > 0.5f)
    {
        uint textureIndex = NonUniformResourceIndex((uint)round(g_scene.lightOptions.w));
        return g_textures[textureIndex].SampleLevel(g_linearSampler, EnvironmentUv(direction), 0.0f).rgb *
            g_scene.environmentTint.rgb * g_scene.environmentOptions.y;
    }
    float3 fallback = g_scene.skyColor.rgb * g_scene.skyColor.a;
    if (g_scene.skyOptions.w < 0.5f)
    {
        return fallback;
    }
    float y = direction.y;
    float horizonBlend = saturate(y * 0.5f + 0.5f);
    float3 sky = lerp(g_scene.skyGroundColor.rgb, g_scene.skyHorizonColor.rgb, smoothstep(-g_scene.skyOptions.z, 0.15f, y));
    sky = lerp(sky, g_scene.skyZenithColor.rgb, horizonBlend * horizonBlend);
    float3 sunDirection = normalize(-g_scene.lightDirection.xyz);
    float sunDot = saturate(dot(direction, sunDirection));
    float sunSize = max(g_scene.skyOptions.y, 0.001f);
    float sunDisk = smoothstep(cos(sunSize * 2.0f), cos(sunSize), sunDot);
    return sky * g_scene.skyColor.a +
        g_scene.lightColor.rgb * g_scene.skyOptions.x * sunDisk +
        fallback * 0.05f;
}

float3 SampleUniformSphere(float2 sample)
{
    float y = 1.0f - 2.0f * sample.x;
    float radius = sqrt(saturate(1.0f - y * y));
    float phi = 2.0f * PI * sample.y;
    return float3(radius * sin(phi), y, radius * cos(phi));
}

float2 CanonicalReservoirUv(float2 uv)
{
    // Match RTXDI_StreamSample's 2x16-bit truncation before any target/PDF
    // evaluation. Otherwise Pass A finalizes one sample while Pass B and
    // history reconstruct a different one from the packed reservoir.
    uint2 packed = (uint2)(saturate(uv) * 65535.0f);
    return float2(packed) / 65535.0f;
}

float2 PrepareReservoirLightUv(uint lightIndex, float2 randomUv)
{
    if (lightIndex == RtxdiEnvironmentLightIndex)
    {
        float3 direction;
        if (g_scene.environmentOptions.x > 0.5f)
        {
            float ignoredPdf;
            SampleEnvironmentDirection(randomUv, direction, ignoredPdf);
        }
        else
        {
            direction = SampleUniformSphere(randomUv);
        }
        // Store a reconstructible direction, not the random numbers consumed
        // by a potentially much larger environment alias table.
        return CanonicalReservoirUv(EnvironmentUv(direction));
    }
    return CanonicalReservoirUv(randomUv);
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

float3 CameraRayFromInverseViewProjection(
    uint2 pixel,
    float2 jitterPixels,
    row_major float4x4 inverseViewProjection)
{
    uint2 dimensions = RenderDimensions();
    float2 uv = (float2(pixel) + 0.5f.xx + jitterPixels) / float2(dimensions);
    float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    float4 nearPoint = mul(float4(ndc, 0.0f, 1.0f), inverseViewProjection);
    float4 farPoint = mul(float4(ndc, 1.0f, 1.0f), inverseViewProjection);
    nearPoint.xyz /= max(abs(nearPoint.w), 1.0e-8f);
    farPoint.xyz /= max(abs(farPoint.w), 1.0e-8f);
    return normalize(farPoint.xyz - nearPoint.xyz);
}

float3 CameraRay(uint2 pixel, float2 jitterPixels)
{
    return CameraRayFromInverseViewProjection(
        pixel,
        jitterPixels,
        g_scene.inverseViewProjection);
}

float3 ReconstructWorldPosition(uint2 pixel, float primaryHitT)
{
    float3 rayDirection = CameraRay(pixel, g_scene.jitterOptions.xy);
    return g_scene.cameraPosition.xyz + rayDirection * primaryHitT;
}

float3 ReconstructPreviousWorldPosition(uint2 pixel, float primaryHitT)
{
    float3 rayDirection = CameraRayFromInverseViewProjection(
        pixel,
        g_scene.jitterOptions.zw,
        g_scene.previousInverseViewProjection);
    return g_scene.previousCameraPosition.xyz + rayDirection * primaryHitT;
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
    float4 positionCone = g_primaryPositionCone[pixel];
    float4 incidentDirectionSpread = g_primaryGeometricNormal[pixel];
    surface.worldPosition = surface.valid ? positionCone.xyz : 0.0f.xxx;
    surface.rayConeWidth = 0.0f;
    surface.rayConeSpread = 0.0f;
    surface.viewDirection = surface.valid
        ? normalize(incidentDirectionSpread.xyz)
        : 0.0f.xxx;
    if (surface.valid)
    {
        surface.rayConeSpread = max(incidentDirectionSpread.w, 1.0e-6f);
        surface.rayConeWidth = max(positionCone.w, 0.0f);
    }
    return surface;
}

DiSurface LoadPreviousSurfaceForGi(uint2 pixel, float fallbackMetallic)
{
    float4 aov0 = g_previousDenoiseAov0[pixel];
    float4 aov1 = g_previousDenoiseAov1[pixel];
    float primaryHitT = g_previousDenoiseAov2[pixel].w;
    DiSurface surface;
    surface.viewZ = aov0.w;
    surface.normal = normalize(aov0.xyz * 2.0f - 1.0f);
    surface.baseColor = max(aov1.rgb, 0.0f.xxx);
    surface.roughness = clamp(aov1.w, 0.04f, 1.0f);
    // Metallic is not part of the immutable previous SurfaceGuide. Reuse is
    // already edge-stopped by albedo/roughness, so the current receiver value
    // is the closest stable material approximation for Basic bias correction.
    surface.metallic = saturate(fallbackMetallic);
    surface.identity = g_previousSurfaceIdentity[pixel];
    surface.valid =
        primaryHitT > 0.0f &&
        surface.viewZ > 0.0f &&
        surface.identity != 0u;
    surface.worldPosition = surface.valid
        ? ReconstructPreviousWorldPosition(pixel, primaryHitT)
        : 0.0f.xxx;
    surface.rayConeWidth = 0.0f;
    surface.rayConeSpread = 0.0f;
    surface.viewDirection = normalize(
        g_scene.previousCameraPosition.xyz - surface.worldPosition);
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

uint SelectUnifiedLightIndex(float sample)
{
    float areaProbability;
    float sunProbability;
    float environmentProbability;
    UnifiedLightProbabilities(areaProbability, sunProbability, environmentProbability);
    if (sample < areaProbability && areaProbability > 0.0f)
    {
        uint lightCount = (uint)round(g_scene.lightOptions.x);
        return lightCount > 0u
            ? SelectLightIndex(sample / areaProbability, lightCount)
            : RtxdiEnvironmentLightIndex;
    }
    if (sample < areaProbability + sunProbability && sunProbability > 0.0f)
    {
        return RtxdiSunLightIndex;
    }
    return RtxdiEnvironmentLightIndex;
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
    RtGeometryRecord geometry = g_geometries[light.meshIdentity.x];
    uint first = geometry.indexOffset + light.meshIdentity.y * 3u;
    uint3 indices = uint3(g_indices[first], g_indices[first + 1u], g_indices[first + 2u]);
    float2 texcoord = g_vertices[indices.x].texcoord * barycentrics.x
        + g_vertices[indices.y].texcoord * barycentrics.y
        + g_vertices[indices.z].texcoord * barycentrics.z;
    texcoord = texcoord * material.uvScaleOffset.xy + material.uvScaleOffset.zw;
    float3 emissiveTexture = 1.0f.xxx;
    if (HasMaterialFeature(material, MaterialFeatureEmissiveTexture))
    {
        float uvFootprint = CandidateTriangleUvFootprint(
            indices,
            coneWidthAtHit,
            rayDirection) * max(max(abs(material.uvScaleOffset.x), abs(material.uvScaleOffset.y)), 1.0e-6f);
        uint textureIndex = NonUniformResourceIndex(
            material.textureBaseIndex + TextureSlotEmissive);
        float mipLevel = CandidateTextureMipLevel(
            material.textureBaseIndex + TextureSlotEmissive,
            uvFootprint);
        emissiveTexture =
            g_textures[textureIndex].SampleLevel(
                g_linearSampler,
                texcoord,
                mipLevel).rgb;
    }
    return emissiveTexture * material.emissiveFactor.rgb *
        material.emissiveFactor.a * g_scene.lightOptions.y;
}

DiLightEvaluation EvaluateLight(DiSurface surface, uint lightIndex, float2 uv)
{
    DiLightEvaluation result = (DiLightEvaluation)0;
    if (!surface.valid)
    {
        return result;
    }
    float areaProbability;
    float sunProbability;
    float environmentProbability;
    UnifiedLightProbabilities(areaProbability, sunProbability, environmentProbability);
    float3 viewDirection = surface.viewDirection;
    if (lightIndex == RtxdiSunLightIndex)
    {
        result.direction = normalize(-g_scene.lightDirection.xyz);
        result.distance = g_scene.rayOptions.y;
        result.sourcePdf = sunProbability;
        result.solidAnglePdf = 1.0f;
        if (result.sourcePdf <= 0.0f || dot(surface.normal, result.direction) <= 0.0f)
        {
            return result;
        }
        EvaluateBsdfLobes(surface, viewDirection, result.direction, result.diffuse, result.specular);
        float3 radiance = g_scene.lightColor.rgb * g_scene.lightColor.a;
        result.diffuse *= radiance;
        result.specular *= radiance;
        result.targetPdf = Luminance(max(
            (result.diffuse + result.specular) / result.solidAnglePdf,
            0.0f.xxx));
        result.valid = result.targetPdf > 0.0f && !isnan(result.targetPdf) && !isinf(result.targetPdf);
        return result;
    }
    if (lightIndex == RtxdiEnvironmentLightIndex)
    {
        float directionPdf;
        result.direction = DecodeEnvironmentDirection(uv);
        if (g_scene.environmentOptions.x > 0.5f)
        {
            directionPdf = EnvironmentDirectionPdf(result.direction);
        }
        else
        {
            directionPdf = 1.0f / (4.0f * PI);
        }
        result.distance = g_scene.rayOptions.y;
        result.sourcePdf = environmentProbability;
        result.solidAnglePdf = directionPdf;
        if (result.sourcePdf <= 0.0f || result.solidAnglePdf <= 0.0f ||
            dot(surface.normal, result.direction) <= 0.0f)
        {
            return result;
        }
        EvaluateBsdfLobes(surface, viewDirection, result.direction, result.diffuse, result.specular);
        float3 radiance = EvaluateEnvironment(result.direction);
        float bsdfPdf = EvaluateBsdfPdf(surface, viewDirection, result.direction);
        float lightPdf = result.sourcePdf * result.solidAnglePdf;
        float lightPdf2 = lightPdf * lightPdf;
        float misWeight = lightPdf2 / max(lightPdf2 + bsdfPdf * bsdfPdf, 1.0e-20f);
        result.diffuse *= radiance * misWeight;
        result.specular *= radiance * misWeight;
        result.targetPdf = Luminance(max(
            (result.diffuse + result.specular) / result.solidAnglePdf,
            0.0f.xxx));
        result.valid = result.targetPdf > 0.0f && !isnan(result.targetPdf) && !isinf(result.targetPdf);
        return result;
    }

    uint lightCount = (uint)round(g_scene.lightOptions.x);
    if (lightIndex >= lightCount || areaProbability <= 0.0f)
    {
        return result;
    }

    RtLight light = g_lights[lightIndex];
    float3 edge0 = light.edge0Type.xyz;
    float3 edge1 = light.edge1.xyz;
    float type = light.edge0Type.w;
    float selectionPdf = light.radianceCdf.w;
    if (type >= 2.0f)
    {
        result.sourcePdf = areaProbability * selectionPdf;
        result.solidAnglePdf = 1.0f;
        float3 radiance = light.radianceCdf.rgb * g_scene.lightOptions.z;
        if (type < 4.0f)
        {
            float3 toLight = light.positionArea.xyz - surface.worldPosition;
            float distanceSquared = max(dot(toLight, toLight), 1.0e-8f);
            result.distance = sqrt(distanceSquared);
            result.direction = toLight / result.distance;
            radiance /= distanceSquared;
            if (type < 3.5f)
            {
                float spotCosine = dot(normalize(edge0), -result.direction);
                radiance *= smoothstep(light.edge1.y, light.edge1.x, spotCosine);
            }
        }
        else
        {
            result.distance = g_scene.rayOptions.y;
            result.direction = normalize(-edge0);
        }
        if (result.sourcePdf <= 0.0f || dot(surface.normal, result.direction) <= 0.0f ||
            MaxComponent(radiance) <= 0.0f)
        {
            return result;
        }
        EvaluateBsdfLobes(surface, viewDirection, result.direction, result.diffuse, result.specular);
        result.diffuse *= radiance;
        result.specular *= radiance;
        result.targetPdf = Luminance(max(result.diffuse + result.specular, 0.0f.xxx));
        result.valid = result.targetPdf > 0.0f && !isnan(result.targetPdf) && !isinf(result.targetPdf);
        return result;
    }
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
    float emittedCosine = dot(normalize(cross(edge0, edge1)), -result.direction);
    float lightCosine = type < -0.5f ? abs(emittedCosine) : saturate(emittedCosine);
    if (lightCosine <= 1.0e-5f || selectionPdf <= 0.0f || dot(surface.normal, result.direction) <= 0.0f)
    {
        return result;
    }

    result.sourcePdf = areaProbability * selectionPdf;
    result.solidAnglePdf = distanceSquared /
        max(light.positionArea.w * lightCosine, 1.0e-10f);
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
    float lightPdf = result.sourcePdf * result.solidAnglePdf;
    float lightPdf2 = lightPdf * lightPdf;
    float misWeight = type < 0.5f ? lightPdf2 / max(lightPdf2 + bsdfPdf * bsdfPdf, 1.0e-20f) : 1.0f;
    result.diffuse *= radiance * misWeight;
    result.specular *= radiance * misWeight;
    result.targetPdf = Luminance(max(
        (result.diffuse + result.specular) / result.solidAnglePdf,
        0.0f.xxx));
    result.valid = result.sourcePdf > 0.0f && result.solidAnglePdf > 0.0f &&
        result.targetPdf > 0.0f &&
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
    uint geometryIndex = query.CandidateInstanceID() + query.CandidateGeometryIndex();
    RtGeometryRecord geometry = g_geometries[geometryIndex];
    RtMaterial material = g_materials[geometry.materialIndex];
    // Primary DXR paths bend thick-dielectric rays using Snell's law. Inline
    // shadow/GI visibility deliberately treats glass as clear: bending a
    // visibility segment would require a multi-segment caustic estimator and
    // must not turn unsupported caustics into opaque shadows.
    if (material.transmissionFactor > 0.0f)
    {
        return true;
    }
    if (material.alphaMasked == 0u)
    {
        return false;
    }
    bool hasSeparateAlpha = HasMaterialFeature(material, MaterialFeatureAlphaTexture);
    if (!hasSeparateAlpha && !HasMaterialFeature(material, MaterialFeatureBaseColorTexture))
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
    uv = uv * material.uvScaleOffset.xy + material.uvScaleOffset.zw;
    uint baseColorTexture = material.textureBaseIndex + (hasSeparateAlpha ? TextureSlotAlpha : TextureSlotBaseColor);
    uint textureIndex = NonUniformResourceIndex(baseColorTexture);
    float coneWidthAtHit = surface.rayConeWidth + surface.rayConeSpread * query.CandidateTriangleRayT();
    float uvFootprint = CandidateTriangleUvFootprint(indices, coneWidthAtHit, rayDirection) *
        max(max(abs(material.uvScaleOffset.x), abs(material.uvScaleOffset.y)), 1.0e-6f);
    float mipLevel = CandidateTextureMipLevel(baseColorTexture, uvFootprint);
    float4 alphaSample = g_textures[textureIndex].SampleLevel(g_linearSampler, uv, mipLevel);
    float alpha = material.baseColorFactor.a * (hasSeparateAlpha ? alphaSample.r : alphaSample.a);
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

bool ValidatePreviousSpatialSurface(
    uint2 currentPixel,
    uint2 previousPixel,
    float expectedPreviousDepth)
{
    float4 currentAov0 = g_denoiseAov0[currentPixel];
    float4 currentAov1 = g_denoiseAov1[currentPixel];
    float4 previousAov0 = g_previousDenoiseAov0[previousPixel];
    float4 previousAov1 = g_previousDenoiseAov1[previousPixel];
    if (g_previousDenoiseAov2[previousPixel].w <= 0.0f)
    {
        return false;
    }
    float normalDot = dot(
        normalize(currentAov0.xyz * 2.0f - 1.0f),
        normalize(previousAov0.xyz * 2.0f - 1.0f));
    float relativeDepth = abs(previousAov0.w - expectedPreviousDepth) /
        max(abs(expectedPreviousDepth), 1.0f);
    return normalDot >= g_scene.validationOptions.x &&
        relativeDepth <= g_scene.validationOptions.y &&
        length(currentAov1.rgb - previousAov1.rgb) <=
            max(g_scene.validationOptions.z, 0.1f) &&
        abs(currentAov1.w - previousAov1.w) <=
            max(g_scene.validationOptions.w, 0.1f);
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
    float areaProbability;
    float sunProbability;
    float environmentProbability;
    UnifiedLightProbabilities(areaProbability, sunProbability, environmentProbability);
    if (surface.valid && areaProbability + sunProbability + environmentProbability > 0.0f)
    {
        uint candidateCount = clamp((uint)round(g_scene.restirDiOptions.z), 1u, 4u);
        uint sampleIndex = g_scene.frameOptions.w;
        [loop]
        for (uint candidate = 0u; candidate < candidateCount; ++candidate)
        {
            uint dimensionBase = RtxdiDimensionCandidateBase + candidate * 4u;
            uint lightIndex = SelectUnifiedLightIndex(
                OwenScrambledSobol1D(pixel, sampleIndex, dimensionBase));
            float2 uv = PrepareReservoirLightUv(
                lightIndex,
                OwenScrambledSobol2D(pixel, sampleIndex, dimensionBase + 1u));
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
            float targetAtCurrent = 0.0f;
            if (RTXDI_IsValidDIReservoir(previous))
            {
                uint lightIndex = RTXDI_GetDIReservoirLightIndex(previous);
                DiLightEvaluation evaluation = EvaluateLight(surface, lightIndex, RTXDI_GetDIReservoirSampleUV(previous));
                targetAtCurrent = evaluation.valid ? evaluation.targetPdf : 0.0f;
                acceptedHistory = targetAtCurrent > 0.0f;
                acceptedHistoryAge = previous.age;
            }
            // An invalid reservoir still represents M zero-target candidates.
            // Dropping it would condition reuse on having found a nonzero
            // sample and brighten the estimator by roughly 1 / validRate.
            float selectionCoin = OwenScrambledSobol1D(
                pixel,
                g_scene.frameOptions.w,
                RtxdiDimensionTemporalCoin);
            RTXDI_CombineDIReservoirs(
                result,
                previous,
                selectionCoin,
                targetAtCurrent);
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
    float lightDistance = 0.0f;
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
            RecordQualityRay(pixel, dimensions, QualityRayDiVisibility);
            float visibility = TraceVisibility(surface, evaluation);
            uint reservoirHistoryAge = reservoir.age;
            RTXDI_StoreVisibilityInDIReservoir(reservoir, visibility.xxx, visibility <= 0.0f);
            // Preserve the temporal surface-history length. Visibility is not
            // reused by this path; it is re-evaluated exactly every frame.
            reservoir.age = reservoirHistoryAge;
            float invPdf = RTXDI_GetDIReservoirInvPdf(reservoir);
            float conditionalInvPdf = rcp(max(evaluation.solidAnglePdf, 1.0e-10f));
            diffuse = evaluation.diffuse * invPdf * conditionalInvPdf * visibility;
            specular = evaluation.specular * invPdf * conditionalInvPdf * visibility;
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
            lightDistance = evaluation.distance;
        }
        else
        {
            reservoir = RTXDI_EmptyDIReservoir();
        }
    }
    StoreCurrentReservoir(pixel, reservoir);

    float4 diffuseSignal = g_signalDiffuse[pixel];
    float4 specularSignal = g_signalSpecular[pixel];
    AddSignalContribution(diffuseSignal, diffuse, lightDistance);
    AddSignalContribution(specularSignal, specular, lightDistance);
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
            float targetAtCurrent = 0.0f;
            if (RTXDI_IsValidDIReservoir(candidate))
            {
                DiLightEvaluation evaluation = EvaluateLight(
                    surface,
                    RTXDI_GetDIReservoirLightIndex(candidate),
                    RTXDI_GetDIReservoirSampleUV(candidate));
                targetAtCurrent = evaluation.valid
                    ? evaluation.targetPdf
                    : 0.0f;
            }
            float selectionCoin = OwenScrambledSobol1D(
                pixel,
                sampleIndex,
                RtxdiDimensionSpatialCoinBase + i);
            // Keep M from zero-target neighbors in the normalization. RTXDI
            // reservoirs represent candidate streams, not only selected hits.
            RTXDI_CombineDIReservoirs(
                result,
                candidate,
                selectionCoin,
                targetAtCurrent);
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

static const uint RtxdiDimensionGiLobe = 400u;
static const uint RtxdiDimensionGiDirection = 401u;
static const uint RtxdiDimensionGiLightSelector = 403u;
static const uint RtxdiDimensionGiLightUv = 404u;
static const uint RtxdiDimensionGiTemporalCoin = 406u;
static const uint RtxdiDimensionGiSpatialRotation = 407u;
static const uint RtxdiDimensionGiSpatialCoinBase = 408u;
static const uint GiMiscTemporalAccepted = 1u << 16u;
static const uint GiMiscSpatialAccepted = 1u << 17u;
static const uint GiMiscInitialSelected = 1u << 18u;

float3 GiTangentToWorld(float3 localDirection, float3 normal)
{
    float3 up = abs(normal.z) < 0.999f
        ? float3(0.0f, 0.0f, 1.0f)
        : float3(1.0f, 0.0f, 0.0f);
    float3 tangent = normalize(cross(up, normal));
    float3 bitangent = cross(normal, tangent);
    return normalize(localDirection.x * tangent + localDirection.y * bitangent + localDirection.z * normal);
}

float3 GiSampleCosineHemisphere(float2 sample, float3 normal)
{
    float phi = 2.0f * PI * sample.x;
    float radius = sqrt(sample.y);
    return GiTangentToWorld(
        float3(radius * cos(phi), radius * sin(phi), sqrt(max(1.0f - sample.y, 0.0f))),
        normal);
}

float3 GiSampleGGX(float2 sample, float roughness, float3 normal, float3 viewDirection)
{
    float3 up = abs(normal.z) < 0.999f
        ? float3(0.0f, 0.0f, 1.0f)
        : float3(1.0f, 0.0f, 0.0f);
    float3 tangent = normalize(cross(up, normal));
    float3 bitangent = cross(normal, tangent);
    float3 viewLocal = float3(
        dot(viewDirection, tangent),
        dot(viewDirection, bitangent),
        max(dot(viewDirection, normal), 1.0e-5f));
    float alpha = max(roughness * roughness, 0.001f);
    float3 stretchedView = normalize(float3(alpha * viewLocal.xy, viewLocal.z));
    float lensq = dot(stretchedView.xy, stretchedView.xy);
    float3 basisX = lensq > 1.0e-8f
        ? float3(-stretchedView.y, stretchedView.x, 0.0f) * rsqrt(lensq)
        : float3(1.0f, 0.0f, 0.0f);
    float3 basisY = cross(stretchedView, basisX);
    float radius = sqrt(sample.x);
    float phi = 2.0f * PI * sample.y;
    float diskX = radius * cos(phi);
    float diskY = radius * sin(phi);
    diskY = lerp(
        sqrt(max(1.0f - diskX * diskX, 0.0f)),
        diskY,
        0.5f * (1.0f + stretchedView.z));
    float diskZ = sqrt(max(1.0f - diskX * diskX - diskY * diskY, 0.0f));
    float3 stretchedNormal = diskX * basisX + diskY * basisY + diskZ * stretchedView;
    float3 halfLocal = normalize(float3(
        alpha * stretchedNormal.xy,
        max(stretchedNormal.z, 0.0f)));
    float3 halfVector = normalize(
        halfLocal.x * tangent + halfLocal.y * bitangent + halfLocal.z * normal);
    return normalize(reflect(-viewDirection, halfVector));
}

DiSurface LoadCommittedGiSurface(
    inout RayQuery<RAY_FLAG_NONE> query,
    DiSurface primarySurface,
    float3 rayDirection,
    out float3 reservoirNormal)
{
    reservoirNormal = 0.0f.xxx;
    DiSurface surface = (DiSurface)0;
    if (query.CommittedStatus() != COMMITTED_TRIANGLE_HIT)
    {
        return surface;
    }
    uint geometryIndex = query.CommittedInstanceID() + query.CommittedGeometryIndex();
    uint primitiveIndex = query.CommittedPrimitiveIndex();
    RtGeometryRecord geometry = g_geometries[geometryIndex];
    RtMaterial material = g_materials[geometry.materialIndex];
    uint first = geometry.indexOffset + primitiveIndex * 3u;
    uint3 indices = uint3(g_indices[first], g_indices[first + 1u], g_indices[first + 2u]);
    float2 bary2 = query.CommittedTriangleBarycentrics();
    float3 bary = float3(1.0f - bary2.x - bary2.y, bary2.x, bary2.y);
    MeshVertex v0 = g_vertices[indices.x];
    MeshVertex v1 = g_vertices[indices.y];
    MeshVertex v2 = g_vertices[indices.z];
    float2 texcoord = (v0.texcoord * bary.x + v1.texcoord * bary.y + v2.texcoord * bary.z) *
        material.uvScaleOffset.xy + material.uvScaleOffset.zw;
    float4 baseSample = HasMaterialFeature(material, MaterialFeatureBaseColorTexture)
        ? g_textures[NonUniformResourceIndex(material.textureBaseIndex + TextureSlotBaseColor)]
            .SampleLevel(g_linearSampler, texcoord, 0.0f)
        : 1.0f.xxxx;
    float roughness = material.roughnessFactor;
    if (HasMaterialFeature(material, MaterialFeatureRoughnessTexture))
    {
        float3 roughnessSample =
            g_textures[NonUniformResourceIndex(material.textureBaseIndex + 2u)]
                .SampleLevel(g_linearSampler, texcoord, 0.0f).rgb;
        roughness *= HasMaterialFeature(material, 1u)
            ? roughnessSample.g
            : roughnessSample.r;
    }
    float metallic = material.metallicFactor;
    if (HasMaterialFeature(material, MaterialFeatureMetallicTexture))
    {
        float3 metallicSample =
            g_textures[NonUniformResourceIndex(material.textureBaseIndex + 3u)]
                .SampleLevel(g_linearSampler, texcoord, 0.0f).rgb;
        metallic *= HasMaterialFeature(material, 1u)
            ? metallicSample.b
            : metallicSample.r;
    }
    surface.worldPosition =
        primarySurface.worldPosition + rayDirection * query.CommittedRayT();
    float3 localReservoirNormal = normalize(cross(
        v1.position - v0.position,
        v2.position - v0.position));
    float3 localShadingNormal = normalize(v0.normal * bary.x + v1.normal * bary.y + v2.normal * bary.z);
    RtInstance instance = g_instances[query.CommittedInstanceIndex()];
    reservoirNormal = normalize(float3(
        dot(float4(localReservoirNormal, 0.0f), instance.normalToWorldColumn0),
        dot(float4(localReservoirNormal, 0.0f), instance.normalToWorldColumn1),
        dot(float4(localReservoirNormal, 0.0f), instance.normalToWorldColumn2)));
    reservoirNormal = dot(reservoirNormal, -rayDirection) >= 0.0f
        ? reservoirNormal
        : -reservoirNormal;
    surface.normal = normalize(float3(
        dot(float4(localShadingNormal, 0.0f), instance.normalToWorldColumn0),
        dot(float4(localShadingNormal, 0.0f), instance.normalToWorldColumn1),
        dot(float4(localShadingNormal, 0.0f), instance.normalToWorldColumn2)));
    surface.normal = dot(surface.normal, -rayDirection) >= 0.0f
        ? surface.normal
        : -surface.normal;
    surface.baseColor = max(baseSample.rgb * material.baseColorFactor.rgb, 0.0f.xxx);
    surface.roughness = clamp(roughness, 0.04f, 1.0f);
    surface.metallic = saturate(metallic);
    surface.viewDirection = normalize(-rayDirection);
    surface.rayConeSpread = primarySurface.rayConeSpread + surface.roughness * surface.roughness * 0.25f;
    surface.rayConeWidth = primarySurface.rayConeWidth +
        primarySurface.rayConeSpread * query.CommittedRayT();
    surface.viewZ = 1.0f;
    surface.identity = 1u;
    surface.valid = true;
    return surface;
}

float MatchedGiEmissiveLightPdf(
    uint instanceIndex,
    uint geometryIndex,
    uint primitiveIndex,
    float3 rayOrigin,
    float3 hitPosition,
    float3 rayDirection)
{
    uint lightCount = (uint)round(g_scene.lightOptions.x);
    uint lower = 0u;
    uint upper = lightCount;
    [loop]
    while (lower < upper)
    {
        uint middle = lower + (upper - lower) / 2u;
        uint4 identity = g_lights[middle].meshIdentity;
        bool precedesTarget = identity.x < geometryIndex ||
            (identity.x == geometryIndex && (identity.y < primitiveIndex ||
            (identity.y == primitiveIndex && identity.w < instanceIndex)));
        if (precedesTarget)
        {
            lower = middle + 1u;
        }
        else
        {
            upper = middle;
        }
    }
    if (lower >= lightCount)
    {
        return 0.0f;
    }
    RtLight light = g_lights[lower];
    if (light.edge0Type.w >= 0.5f ||
        light.meshIdentity.x != geometryIndex ||
        light.meshIdentity.y != primitiveIndex ||
        light.meshIdentity.w != instanceIndex ||
        light.radianceCdf.w <= 0.0f ||
        light.positionArea.w <= 0.0f)
    {
        return 0.0f;
    }
    float3 lightNormal = normalize(cross(
        light.edge0Type.xyz,
        light.edge1.xyz));
    float emittedCosine = dot(lightNormal, -rayDirection);
    float lightCosine = light.edge0Type.w < -0.5f ? abs(emittedCosine) : saturate(emittedCosine);
    float3 toLight = hitPosition - rayOrigin;
    float distanceSquared = max(dot(toLight, toLight), 1.0e-8f);
    return lightCosine > 1.0e-6f
        ? (light.radianceCdf.w / light.positionArea.w) *
            distanceSquared / lightCosine
        : 0.0f;
}

float GiBsdfTechniqueMisWeight(float bsdfPdf, float lightPdf)
{
    float bsdfPdf2 = bsdfPdf * bsdfPdf;
    float lightPdf2 = lightPdf * lightPdf;
    return bsdfPdf2 / max(bsdfPdf2 + lightPdf2, 1.0e-20f);
}

float3 LoadCommittedGiEmission(
    inout RayQuery<RAY_FLAG_NONE> query,
    float3 rayOrigin,
    float3 hitPosition,
    float3 rayDirection,
    float bsdfPdf)
{
    if (query.CommittedStatus() != COMMITTED_TRIANGLE_HIT)
    {
        return 0.0f.xxx;
    }
    uint geometryIndex = query.CommittedInstanceID() + query.CommittedGeometryIndex();
    uint primitiveIndex = query.CommittedPrimitiveIndex();
    RtGeometryRecord geometry = g_geometries[geometryIndex];
    RtMaterial material = g_materials[geometry.materialIndex];
    uint first = geometry.indexOffset + primitiveIndex * 3u;
    uint3 indices = uint3(g_indices[first], g_indices[first + 1u], g_indices[first + 2u]);
    float2 bary2 = query.CommittedTriangleBarycentrics();
    float3 bary = float3(1.0f - bary2.x - bary2.y, bary2.x, bary2.y);
    float2 texcoord = g_vertices[indices.x].texcoord * bary.x +
        g_vertices[indices.y].texcoord * bary.y +
        g_vertices[indices.z].texcoord * bary.z;
    texcoord = texcoord * material.uvScaleOffset.xy + material.uvScaleOffset.zw;
    float3 textureEmission = 1.0f.xxx;
    if (HasMaterialFeature(material, MaterialFeatureEmissiveTexture))
    {
        textureEmission =
            g_textures[NonUniformResourceIndex(
                material.textureBaseIndex + TextureSlotEmissive)]
                .SampleLevel(g_linearSampler, texcoord, 0.0f).rgb;
    }
    float3 emission = textureEmission * material.emissiveFactor.rgb *
        material.emissiveFactor.a * g_scene.lightOptions.y;
    if (MaxComponent(emission) <= 0.0f)
    {
        return 0.0f.xxx;
    }
    float areaProbability;
    float sunProbability;
    float environmentProbability;
    UnifiedLightProbabilities(
        areaProbability,
        sunProbability,
        environmentProbability);
    float lightPdf = areaProbability * MatchedGiEmissiveLightPdf(
        query.CommittedInstanceIndex(),
        geometryIndex,
        primitiveIndex,
        rayOrigin,
        hitPosition,
        rayDirection);
    return emission * GiBsdfTechniqueMisWeight(bsdfPdf, lightPdf);
}

RTXDI_GIReservoir GenerateInitialGiReservoir(
    uint2 pixel,
    DiSurface primarySurface,
    uint visibilityRayKind)
{
    RTXDI_GIReservoir reservoir = RTXDI_EmptyGIReservoir();
    if (!primarySurface.valid)
    {
        return reservoir;
    }
    // One proposal was requested for this valid primary surface. Preserve its
    // zero-target mass when the sampled direction is rejected or misses;
    // conditioning reuse on secondary hits would brighten GI by 1 / hitRate.
    reservoir.M = 1u;
    uint sampleIndex = g_scene.frameOptions.w;
    float specularProbability = BsdfSpecularProbability(
        primarySurface,
        primarySurface.viewDirection);
    bool sampleSpecular = OwenScrambledSobol1D(
        pixel, sampleIndex, RtxdiDimensionGiLobe) < specularProbability;
    float2 directionSample = OwenScrambledSobol2D(
        pixel, sampleIndex, RtxdiDimensionGiDirection);
    float3 direction = sampleSpecular
        ? GiSampleGGX(
            directionSample,
            primarySurface.roughness,
            primarySurface.normal,
            primarySurface.viewDirection)
        : GiSampleCosineHemisphere(directionSample, primarySurface.normal);
    float samplePdf = EvaluateBsdfPdf(
        primarySurface,
        primarySurface.viewDirection,
        direction);
    if (samplePdf <= 1.0e-8f || dot(primarySurface.normal, direction) <= 0.0f)
    {
        return reservoir;
    }

    RayDesc ray;
    ray.Origin = primarySurface.worldPosition + primarySurface.normal * g_scene.rayOptions.x;
    ray.Direction = direction;
    ray.TMin = g_scene.rayOptions.x;
    ray.TMax = g_scene.rayOptions.y;
    RayQuery<RAY_FLAG_NONE> query;
    RecordQualityRay(pixel, RenderDimensions(), QualityRaySecondary);
    query.TraceRayInline(g_sceneAs, RAY_FLAG_NONE, 0xff, ray);
    while (query.Proceed())
    {
        if (query.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE &&
            !IsAlphaTransparentCandidate(query, primarySurface, direction))
        {
            query.CommitNonOpaqueTriangleHit();
        }
    }
    float3 reservoirNormal;
    DiSurface secondarySurface = LoadCommittedGiSurface(
        query,
        primarySurface,
        direction,
        reservoirNormal);
    if (!secondarySurface.valid)
    {
        // A BSDF continuation that escapes to the environment is a valid
        // indirect proposal. Pair it with the environment-light technique so
        // combined DI+GI remains complementary instead of either dropping or
        // double-counting sky energy.
        float areaProbability;
        float sunProbability;
        float environmentProbability;
        UnifiedLightProbabilities(
            areaProbability,
            sunProbability,
            environmentProbability);
        float directionPdf = g_scene.environmentOptions.x > 0.5f
            ? EnvironmentDirectionPdf(direction)
            : 1.0f / (4.0f * PI);
        float lightPdf = environmentProbability * directionPdf;
        float3 environmentRadiance =
            EvaluateEnvironment(direction) *
            GiBsdfTechniqueMisWeight(samplePdf, lightPdf);
        if (MaxComponent(environmentRadiance) <= 0.0f ||
            any(isnan(environmentRadiance)) ||
            any(isinf(environmentRadiance)))
        {
            return reservoir;
        }
        return RTXDI_MakeGIReservoir(
            primarySurface.worldPosition + direction * g_scene.rayOptions.y,
            -direction,
            environmentRadiance,
            samplePdf);
    }

    float3 incomingRadiance = LoadCommittedGiEmission(
        query,
        primarySurface.worldPosition,
        secondarySurface.worldPosition,
        direction,
        samplePdf);
    uint lightIdentity = SelectUnifiedLightIndex(OwenScrambledSobol1D(
        pixel, sampleIndex, RtxdiDimensionGiLightSelector));
    float2 lightUv = PrepareReservoirLightUv(
        lightIdentity,
        OwenScrambledSobol2D(
            pixel, sampleIndex, RtxdiDimensionGiLightUv));
    DiLightEvaluation light = EvaluateLight(secondarySurface, lightIdentity, lightUv);
    if (light.valid)
    {
        RecordQualityRay(pixel, RenderDimensions(), visibilityRayKind);
        incomingRadiance += (light.diffuse + light.specular) *
            TraceVisibility(secondarySurface, light) /
            max(light.sourcePdf * light.solidAnglePdf, 1.0e-8f);
    }
    incomingRadiance = max(incomingRadiance, 0.0f.xxx);
    if (any(isnan(incomingRadiance)) || any(isinf(incomingRadiance)))
    {
        return reservoir;
    }
    return RTXDI_MakeGIReservoir(
        secondarySurface.worldPosition,
        reservoirNormal,
        incomingRadiance,
        samplePdf);
}

RTXDI_GIReservoir LoadGiHistory(uint2 pixel, out uint miscFlags)
{
    return RTXDI_UnpackGIReservoir(
        g_restirGiHistory[ReservoirPointer(pixel)],
        miscFlags);
}

RTXDI_GIReservoir LoadGiHistory(uint2 pixel)
{
    uint miscFlags;
    return LoadGiHistory(pixel, miscFlags);
}

float GiTargetPdf(DiSurface primarySurface, RTXDI_GIReservoir reservoir)
{
    if (!primarySurface.valid || !RTXDI_IsValidGIReservoir(reservoir))
    {
        return 0.0f;
    }
    float3 toSecondary = reservoir.position - primarySurface.worldPosition;
    float distanceSquared = dot(toSecondary, toSecondary);
    if (distanceSquared <= 1.0e-8f)
    {
        return 0.0f;
    }
    float3 direction = toSecondary * rsqrt(distanceSquared);
    float3 diffuse;
    float3 specular;
    EvaluateBsdfLobes(
        primarySurface,
        primarySurface.viewDirection,
        direction,
        diffuse,
        specular);
    return Luminance(max((diffuse + specular) * reservoir.radiance, 0.0f.xxx));
}

bool GiReuseJacobian(
    float3 receiverPosition,
    float3 sourceReceiverPosition,
    RTXDI_GIReservoir reservoir,
    out float jacobian)
{
    jacobian = RTXDI_CalculateJacobian(
        receiverPosition,
        sourceReceiverPosition,
        reservoir.position,
        reservoir.normal);
    // Match the RTXDI bridge contract: reject a domain change larger than one
    // order of magnitude and clamp accepted Jacobians to control variance.
    if (jacobian < 0.1f || jacobian > 10.0f ||
        isnan(jacobian) || isinf(jacobian))
    {
        jacobian = 0.0f;
        return false;
    }
    jacobian = clamp(jacobian, 1.0f / 3.0f, 3.0f);
    return true;
}

float GiFinalMisInitialWeight(
    float3 roughDiffuse,
    float3 roughSpecular,
    float3 trueDiffuse,
    float3 trueSpecular)
{
    const float maximumBrdf = 1.0e4f;
    float3 roughBrdf = clamp(
        roughDiffuse + roughSpecular,
        1.0e-4f.xxx,
        maximumBrdf.xxx);
    float3 trueBrdf = clamp(
        trueDiffuse + trueSpecular,
        0.0f.xxx,
        maximumBrdf.xxx);
    float weight = saturate(
        Luminance(trueBrdf) /
        max(Luminance(trueBrdf + roughBrdf), 1.0e-8f));
    return weight * weight * weight;
}

float GiFinalVisibility(
    uint2 pixel,
    DiSurface primarySurface,
    RTXDI_GIReservoir reservoir)
{
    float3 toSecondary = reservoir.position - primarySurface.worldPosition;
    float distanceToSecondary = length(toSecondary);
    if (distanceToSecondary <= g_scene.rayOptions.x * 2.0f)
    {
        return 0.0f;
    }
    DiLightEvaluation visibilityRay = (DiLightEvaluation)0;
    visibilityRay.direction = toSecondary / distanceToSecondary;
    visibilityRay.distance = distanceToSecondary;
    RecordQualityRay(pixel, RenderDimensions(), QualityRayGiVisibility);
    return TraceVisibility(primarySurface, visibilityRay);
}

[numthreads(8, 8, 1)]
void RtxdiGiInitialCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint2 pixel = dispatchThreadId.xy;
    uint2 dimensions = RenderDimensions();
    if (any(pixel >= dimensions))
    {
        return;
    }
    RTXDI_GIReservoir reservoir = GenerateInitialGiReservoir(
        pixel,
        LoadCurrentSurface(pixel),
        QualityRayGiVisibility);
    RTXDI_StoreGIReservoir(reservoir, ReservoirParameters(), pixel, 0u);
}

[numthreads(8, 8, 1)]
void RtxdiGiFusedCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint2 pixel = dispatchThreadId.xy;
    uint2 dimensions = RenderDimensions();
    if (any(pixel >= dimensions))
    {
        return;
    }
    DiSurface surface = LoadCurrentSurface(pixel);
    RTXDI_GIReservoir initial = RTXDI_UnpackGIReservoir(
        g_restirGiCurrent[ReservoirPointer(pixel)]);
    RTXDI_GIReservoir result = RTXDI_EmptyGIReservoir();
    float selectedTarget = GiTargetPdf(surface, initial);
    bool selectedInitial = RTXDI_CombineGIReservoirs(
        result,
        initial,
        0.5f,
        selectedTarget);
    bool temporalAccepted = false;
    bool spatialAccepted = false;
    uint selectedAge = 0u;
    static const uint GiInitialSource = 0xffffffffu;
    uint selectedSource = GiInitialSource;
    uint acceptedSourceMask = 0u;
    uint historyDomains = (uint)round(g_scene.environmentOptions.w);
    bool historyValid =
        g_scene.restirOptions.x > 0.5f &&
        g_scene.restirStabilityOptions.x > 0.5f &&
        (historyDomains & (HISTORY_DOMAIN_SURFACE | HISTORY_DOMAIN_LIGHTING)) ==
            (HISTORY_DOMAIN_SURFACE | HISTORY_DOMAIN_LIGHTING);
    float4 motion = g_denoiseAov2[pixel];
    float2 jitterDelta = g_scene.jitterOptions.xy - g_scene.jitterOptions.zw;
    int2 previousPixel = int2(round(
        float2(pixel) + motion.xy * float2(dimensions) + jitterDelta));
    float expectedPreviousDepth = surface.viewZ + motion.z;
    bool previousPixelInBounds =
        all(previousPixel >= 0) &&
        all(previousPixel < int2(dimensions));

    if (surface.valid && historyValid)
    {
        if (previousPixelInBounds &&
            ValidatePreviousSurface(
                pixel,
                uint2(previousPixel),
                expectedPreviousDepth))
        {
            uint previousMisc;
            RTXDI_GIReservoir previous = LoadGiHistory(uint2(previousPixel), previousMisc);
            previous.M = min(previous.M, (uint)max(round(g_scene.restirOptions.w), 1.0f));
            previous.age = min(previous.age + 1u, 255u);
            if (previous.age <= (uint)max(round(g_scene.restirStabilityOptions.w), 1.0f))
            {
                float target = GiTargetPdf(surface, previous);
                float jacobian = 1.0f;
                bool zeroTargetStream =
                    previous.weightSum <= 0.0f ||
                    target <= 0.0f;
                float previousHitT = g_previousDenoiseAov2[uint2(previousPixel)].w;
                bool validJacobian = zeroTargetStream ||
                    GiReuseJacobian(
                        surface.worldPosition,
                        ReconstructPreviousWorldPosition(
                            uint2(previousPixel),
                            previousHitT),
                        previous,
                        jacobian);
                if (validJacobian)
                {
                    // Temporal GI converts the finalized donor reservoir into
                    // the current receiver's solid-angle measure before it is
                    // denormalized by CombineGIReservoirs.
                    previous.weightSum *= jacobian;
                    bool selected = RTXDI_CombineGIReservoirs(
                        result,
                        previous,
                        OwenScrambledSobol1D(
                            pixel,
                            g_scene.frameOptions.w,
                            RtxdiDimensionGiTemporalCoin),
                        target);
                    if (selected)
                    {
                        selectedTarget = target;
                        selectedInitial = false;
                        selectedAge = previous.age;
                        selectedSource = 0u;
                    }
                    acceptedSourceMask |= 1u;
                    temporalAccepted = target > 0.0f;
                }
            }
        }
    }

    uint spatialPasses = clamp((uint)round(g_scene.restirOptions.y), 0u, 4u);
    uint spatialSamples = min(spatialPasses * 2u, 8u);
    uint rotation = uint(OwenScrambledSobol1D(
        pixel,
        g_scene.frameOptions.w,
        RtxdiDimensionGiSpatialRotation) * 16.0f) & 15u;
    float radiusScale = max(g_scene.restirOptions.z / 16.0f, 0.25f);
    if (surface.valid && historyValid && previousPixelInBounds &&
        spatialSamples > 0u)
    {
        [loop]
        for (uint sampleIndex = 0u; sampleIndex < spatialSamples; ++sampleIndex)
        {
            int2 offset = int2(round(float2(
                SpatialOffsets[(sampleIndex + rotation) & 15u]) * radiusScale));
            if (all(offset == 0))
            {
                offset.x = 1;
            }
            // Every source below is a previous-frame reservoir. Sample it
            // around the reprojected previous pixel and validate it against
            // the previous SurfaceGuide at the identical coordinate.
            int2 neighbor = previousPixel + offset;
            if (any(neighbor < 0) || any(neighbor >= int2(dimensions)) ||
                !ValidatePreviousSpatialSurface(
                    pixel,
                    uint2(neighbor),
                    expectedPreviousDepth))
            {
                continue;
            }
            uint neighborMisc;
            RTXDI_GIReservoir candidate = LoadGiHistory(uint2(neighbor), neighborMisc);
            candidate.M = min(candidate.M, (uint)max(round(g_scene.restirOptions.w), 1.0f));
            candidate.age = min(candidate.age + 1u, 255u);
            if (candidate.age >
                (uint)max(round(g_scene.restirStabilityOptions.w), 1.0f))
            {
                continue;
            }
            float target = GiTargetPdf(surface, candidate);
            float jacobian = 1.0f;
            bool zeroTargetStream =
                candidate.weightSum <= 0.0f ||
                target <= 0.0f;
            float neighborHitT = g_previousDenoiseAov2[uint2(neighbor)].w;
            if (!zeroTargetStream &&
                !GiReuseJacobian(
                    surface.worldPosition,
                    ReconstructPreviousWorldPosition(
                        uint2(neighbor),
                        neighborHitT),
                    candidate,
                    jacobian))
            {
                continue;
            }
            bool selected = RTXDI_CombineGIReservoirs(
                result,
                candidate,
                OwenScrambledSobol1D(
                    pixel,
                    g_scene.frameOptions.w,
                    RtxdiDimensionGiSpatialCoinBase + sampleIndex),
                target * jacobian);
            if (selected)
            {
                // The reservoir is normalized in the receiver's target
                // measure. The Jacobian only converts the donor stream.
                selectedTarget = target;
                selectedInitial = false;
                selectedAge = candidate.age;
                selectedSource = sampleIndex + 1u;
            }
            acceptedSourceMask |= 1u << (sampleIndex + 1u);
            spatialAccepted = spatialAccepted || target > 0.0f;
        }
    }

    uint maxM = (uint)max(round(g_scene.restirOptions.w), 1.0f);
    uint normalizationM = max(result.M, 1u);
    result.age = selectedInitial ? 0u : selectedAge;
    float normalizationNumerator = 1.0f;
    float normalizationDenominator =
        selectedTarget * (float)normalizationM;
    if (selectedTarget > 0.0f && result.weightSum > 0.0f)
    {
        // RTXDI Basic bias correction evaluates the selected sample in every
        // candidate stream's receiver domain. This prevents a visible or
        // high-target donor from being normalized as though all reused
        // receivers could have generated it equally well.
        float pi = selectedTarget;
        float piSum = selectedTarget * (float)initial.M;
        if ((acceptedSourceMask & 1u) != 0u)
        {
            RTXDI_GIReservoir donor = LoadGiHistory(uint2(previousPixel));
            donor.M = min(donor.M, maxM);
            DiSurface donorSurface = LoadPreviousSurfaceForGi(
                uint2(previousPixel),
                surface.metallic);
            float ps = GiTargetPdf(donorSurface, result);
            if (g_scene.restirStabilityOptions.z > 0.5f && ps > 0.0f)
            {
                ps *= GiFinalVisibility(pixel, donorSurface, result);
            }
            if (selectedSource == 0u)
            {
                pi = ps;
            }
            piSum += ps * (float)donor.M;
        }
        [loop]
        for (uint sampleIndex = 0u; sampleIndex < spatialSamples; ++sampleIndex)
        {
            uint sourceBit = 1u << (sampleIndex + 1u);
            if ((acceptedSourceMask & sourceBit) == 0u)
            {
                continue;
            }
            int2 offset = int2(round(float2(
                SpatialOffsets[(sampleIndex + rotation) & 15u]) * radiusScale));
            if (all(offset == 0))
            {
                offset.x = 1;
            }
            uint2 donorPixel = uint2(previousPixel + offset);
            RTXDI_GIReservoir donor = LoadGiHistory(donorPixel);
            donor.M = min(donor.M, maxM);
            DiSurface donorSurface = LoadPreviousSurfaceForGi(
                donorPixel,
                surface.metallic);
            float ps = GiTargetPdf(donorSurface, result);
            if (g_scene.restirStabilityOptions.z > 0.5f && ps > 0.0f)
            {
                ps *= GiFinalVisibility(pixel, donorSurface, result);
            }
            if (selectedSource == sampleIndex + 1u)
            {
                pi = ps;
            }
            piSum += ps * (float)donor.M;
        }
        normalizationNumerator = pi;
        normalizationDenominator = selectedTarget * piSum;
    }
    RTXDI_FinalizeGIResampling(
        result,
        normalizationNumerator,
        normalizationDenominator);
    // weightSum is finalized against the complete stream above. Cap only the
    // reusable history count; truncating M before normalization without
    // scaling the accumulated RIS weight amplified center + neighbor streams.
    result.M = min(result.M, maxM);
    uint miscFlags =
        (temporalAccepted ? GiMiscTemporalAccepted : 0u) |
        (spatialAccepted ? GiMiscSpatialAccepted : 0u) |
        (selectedInitial ? GiMiscInitialSelected : 0u);
    RTXDI_StoreGIReservoir(result, miscFlags, ReservoirParameters(), pixel, 0u);

    float3 diffuse = 0.0f.xxx;
    float3 specular = 0.0f.xxx;
    float diffuseHitDistance = 0.0f;
    float specularHitDistance = 0.0f;
    if (surface.valid && RTXDI_IsValidGIReservoir(result) &&
        result.weightSum > 0.0f)
    {
        float3 resultOffset = result.position - surface.worldPosition;
        float resultHitDistance = length(resultOffset);
        float3 direction = resultOffset / max(resultHitDistance, 1.0e-8f);
        EvaluateBsdfLobes(
            surface,
            surface.viewDirection,
            direction,
            diffuse,
            specular);
        float visibility = GiFinalVisibility(pixel, surface, result);
        float3 resampledScale = result.radiance * result.weightSum * visibility;
        float finalWeight = 1.0f;
        float initialWeight = 0.0f;
        if (RTXDI_IsValidGIReservoir(initial) &&
            initial.weightSum > 0.0f)
        {
            float3 initialOffset = initial.position - surface.worldPosition;
            float initialHitDistance = length(initialOffset);
            float3 initialDirection =
                initialOffset / max(initialHitDistance, 1.0e-8f);
            float3 initialDiffuse;
            float3 initialSpecular;
            EvaluateBsdfLobes(
                surface,
                surface.viewDirection,
                initialDirection,
                initialDiffuse,
                initialSpecular);
            DiSurface roughSurface = surface;
            roughSurface.roughness = max(roughSurface.roughness, 0.3f);
            float3 roughResampledDiffuse;
            float3 roughResampledSpecular;
            EvaluateBsdfLobes(
                roughSurface,
                roughSurface.viewDirection,
                direction,
                roughResampledDiffuse,
                roughResampledSpecular);
            float3 roughInitialDiffuse;
            float3 roughInitialSpecular;
            EvaluateBsdfLobes(
                roughSurface,
                roughSurface.viewDirection,
                initialDirection,
                roughInitialDiffuse,
                roughInitialSpecular);
            // RTXDI final MIS is a BRDF-overlap heuristic. A finalized
            // reservoir weight is not a proposal PDF and must not be inverted
            // and fed to a power heuristic.
            finalWeight = 1.0f - GiFinalMisInitialWeight(
                roughResampledDiffuse,
                roughResampledSpecular,
                diffuse,
                specular);
            initialWeight = GiFinalMisInitialWeight(
                roughInitialDiffuse,
                roughInitialSpecular,
                initialDiffuse,
                initialSpecular);
            float3 resampledDiffuse =
                diffuse * resampledScale * finalWeight;
            float3 initialDiffuseContribution =
                initialDiffuse * initial.radiance * initial.weightSum * initialWeight;
            float3 resampledSpecular =
                specular * resampledScale * finalWeight;
            float3 initialSpecularContribution =
                initialSpecular * initial.radiance * initial.weightSum * initialWeight;
            diffuse = resampledDiffuse + initialDiffuseContribution;
            specular = resampledSpecular + initialSpecularContribution;
            diffuseHitDistance =
                Luminance(initialDiffuseContribution) > Luminance(resampledDiffuse)
                ? initialHitDistance
                : resultHitDistance;
            specularHitDistance =
                Luminance(initialSpecularContribution) > Luminance(resampledSpecular)
                ? initialHitDistance
                : resultHitDistance;
        }
        else
        {
            diffuse *= resampledScale;
            specular *= resampledScale;
            diffuseHitDistance = resultHitDistance;
            specularHitDistance = resultHitDistance;
        }
    }
    float contributionLimit = max(g_scene.giOptions.y, 0.0f);
    float contributionLuminance = Luminance(diffuse + specular);
    float inputContributionEnergy = contributionLuminance;
    if (contributionLimit > 0.0f && contributionLuminance > contributionLimit)
    {
        float compression = contributionLimit / max(contributionLuminance, 1.0e-6f);
        diffuse *= compression;
        specular *= compression;
    }
    float outputContributionEnergy = Luminance(diffuse + specular);
    float4 diffuseSignal = g_signalDiffuse[pixel];
    float4 specularSignal = g_signalSpecular[pixel];
    AddSignalContribution(
        diffuseSignal,
        diffuse,
        diffuseHitDistance);
    AddSignalContribution(
        specularSignal,
        specular,
        specularHitDistance);
    g_signalDiffuse[pixel] = diffuseSignal;
    g_signalSpecular[pixel] = specularSignal;
    if (QualityContributionEnabled(dimensions))
    {
        float2 pathAndDirectEnergy =
            UnpackQualityContributionEnergy(g_qualityContribution[pixel]);
        g_qualityContribution[pixel] = PackQualityContributionEnergy(
            pathAndDirectEnergy.x + inputContributionEnergy,
            pathAndDirectEnergy.y + outputContributionEnergy);
    }

    uint debugMode = (uint)round(g_scene.debugOptions.x);
    float3 debugColor = -1.0f.xxx;
    if (debugMode == 49u)
    {
        debugColor = saturate(result.weightSum / 16.0f).xxx;
    }
    else if (debugMode == 50u)
    {
        debugColor = saturate((float)result.age /
            max(g_scene.restirStabilityOptions.w, 1.0f)).xxx;
    }
    else if (debugMode == 51u)
    {
        debugColor = (RTXDI_IsValidGIReservoir(result) ? 1.0f : 0.0f).xxx;
    }
    else if (debugMode == 52u)
    {
        debugColor = (temporalAccepted ? 1.0f : 0.0f).xxx;
    }
    else if (debugMode == 53u)
    {
        debugColor = selectedInitial
            ? float3(0.1f, 0.4f, 1.0f)
            : float3(1.0f, 0.35f, 0.05f);
    }
    if (debugColor.x >= 0.0f)
    {
        g_postDenoiseHdr[pixel] = float4(debugColor, 1.0f);
        g_accumulation[pixel] = float4(debugColor, 1.0f);
        g_output[pixel] = float4(debugColor, 1.0f);
    }
    else if ((uint)round(g_scene.debugOptions.x) == 0u &&
        g_scene.performanceOptions.w < 0.5f)
    {
        float3 hdr = max(g_postDenoiseHdr[pixel].rgb + diffuse + specular, 0.0f.xxx);
        g_postDenoiseHdr[pixel] = float4(hdr, 1.0f);
        g_accumulation[pixel] = float4(hdr, 1.0f);
        g_output[pixel] = float4(Tonemap(hdr), 1.0f);
    }
}

// ReSTIR PT uses the same renderer-native path tracer bridge as GI initial
// sampling, but preserves the replay seed, path length, reconnection PDF and
// target function in the official 64-byte RTXDI PT reservoir ABI.
RTXDI_ReservoirBufferParameters PtReservoirParameters()
{
    uint2 dimensions = RenderDimensions();
    uint2 reservoirDimensions = uint2((dimensions.x + 1u) >> 1u, dimensions.y);
    uint blockRows =
        (reservoirDimensions.x + RTXDI_RESERVOIR_BLOCK_SIZE - 1u) /
        RTXDI_RESERVOIR_BLOCK_SIZE;
    uint blockColumns =
        (reservoirDimensions.y + RTXDI_RESERVOIR_BLOCK_SIZE - 1u) /
        RTXDI_RESERVOIR_BLOCK_SIZE;
    RTXDI_ReservoirBufferParameters parameters;
    parameters.reservoirBlockRowPitch =
        blockRows * RTXDI_RESERVOIR_BLOCK_SIZE * RTXDI_RESERVOIR_BLOCK_SIZE;
    parameters.reservoirArrayPitch =
        parameters.reservoirBlockRowPitch * blockColumns;
    parameters.pad1 = 0u;
    parameters.pad2 = 0u;
    return parameters;
}

uint PtCheckerboardField(uint frameIndex)
{
    return (frameIndex & 1u) + 1u;
}

bool PtPixelIsActive(uint2 pixel, uint checkerboardField)
{
    uint2 reservoirPosition =
        RTXDI_PixelPosToReservoirPos(pixel, checkerboardField);
    return all(
        RTXDI_ReservoirPosToPixelPos(reservoirPosition, checkerboardField) ==
        pixel);
}

uint PtReservoirPointer(uint2 pixel, uint checkerboardField)
{
    uint2 reservoirPosition =
        RTXDI_PixelPosToReservoirPos(pixel, checkerboardField);
    return RTXDI_ReservoirPositionToPointer(
        PtReservoirParameters(),
        reservoirPosition,
        0u);
}

RTXDI_PTReservoir RAB_PathTrace(uint2 pixel, DiSurface primarySurface)
{
    RTXDI_GIReservoir pathSample =
        GenerateInitialGiReservoir(
            pixel,
            primarySurface,
            QualityRayPtVisibility);
    if (!RTXDI_IsValidGIReservoir(pathSample) ||
        pathSample.weightSum <= 0.0f)
    {
        return RTXDI_EmptyPTReservoir();
    }
    float sourcePdf = rcp(pathSample.weightSum);
    float3 target = 0.0f.xxx;
    float3 direction =
        normalize(pathSample.position - primarySurface.worldPosition);
    float3 diffuse;
    float3 specular;
    EvaluateBsdfLobes(
        primarySurface,
        primarySurface.viewDirection,
        direction,
        diffuse,
        specular);
    target = max((diffuse + specular) * pathSample.radiance, 0.0f.xxx);
    uint replaySeed = Hash(
        pixel.x ^ (pixel.y * 0x9e3779b9u) ^
        (g_scene.frameOptions.w * 0x85ebca6bu));
    return RTXDI_MakePTReservoir(
        target,
        replaySeed,
        0u,
        1u,
        2u,
        1.0f,
        sourcePdf,
        pathSample.position,
        pathSample.normal,
        pathSample.radiance,
        sourcePdf);
}

float3 PtTarget(DiSurface surface, RTXDI_PTReservoir reservoir)
{
    if (!surface.valid || !RTXDI_IsValidPTReservoir(reservoir))
    {
        return 0.0f.xxx;
    }
    float3 toSample =
        reservoir.TranslatedWorldPosition - surface.worldPosition;
    if (dot(toSample, toSample) <= 1.0e-8f)
    {
        return 0.0f.xxx;
    }
    float3 direction = normalize(toSample);
    float3 diffuse;
    float3 specular;
    EvaluateBsdfLobes(
        surface,
        surface.viewDirection,
        direction,
        diffuse,
        specular);
    return max((diffuse + specular) * reservoir.Radiance, 0.0f.xxx);
}

float PtFinalVisibility(
    uint2 pixel,
    DiSurface surface,
    RTXDI_PTReservoir reservoir)
{
    float3 toSample =
        reservoir.TranslatedWorldPosition - surface.worldPosition;
    float distanceToSample = length(toSample);
    if (distanceToSample <= g_scene.rayOptions.x * 2.0f)
    {
        return 0.0f;
    }
    DiLightEvaluation visibilityRay = (DiLightEvaluation)0;
    visibilityRay.direction = toSample / distanceToSample;
    visibilityRay.distance = distanceToSample;
    RecordQualityRay(pixel, RenderDimensions(), QualityRayPtVisibility);
    return TraceVisibility(surface, visibilityRay);
}

[numthreads(8, 8, 1)]
void RtxdiPtInitialCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint2 pixel = dispatchThreadId.xy;
    uint2 dimensions = RenderDimensions();
    uint checkerboardField =
        PtCheckerboardField(g_scene.frameOptions.w);
    if (any(pixel >= dimensions) ||
        !PtPixelIsActive(pixel, checkerboardField))
    {
        return;
    }
    RTXDI_PTReservoir reservoir =
        RAB_PathTrace(pixel, LoadCurrentSurface(pixel));
    g_restirPtCurrent[PtReservoirPointer(pixel, checkerboardField)] =
        RTXDI_PackPTReservoir(reservoir);
}

[numthreads(8, 8, 1)]
void RtxdiPtFusedCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint2 pixel = dispatchThreadId.xy;
    uint2 dimensions = RenderDimensions();
    uint frameIndex = g_scene.frameOptions.w;
    uint currentField = PtCheckerboardField(frameIndex);
    if (any(pixel >= dimensions) ||
        !PtPixelIsActive(pixel, currentField))
    {
        return;
    }

    DiSurface surface = LoadCurrentSurface(pixel);
    RTXDI_PTReservoir initial = RTXDI_UnpackPTReservoir(
        g_restirPtCurrent[PtReservoirPointer(pixel, currentField)]);
    RTXDI_PTReservoir result = RTXDI_EmptyPTReservoir();
    float3 selectedTarget = PtTarget(surface, initial);
    bool selectedInitial = CombineReservoirs(
        result,
        initial,
        0.5f,
        selectedTarget);

    uint historyDomains = (uint)round(g_scene.environmentOptions.w);
    bool historyValid =
        g_scene.restirOptions.x > 0.5f &&
        g_scene.restirStabilityOptions.x > 0.5f &&
        (historyDomains & (HISTORY_DOMAIN_SURFACE | HISTORY_DOMAIN_LIGHTING)) ==
            (HISTORY_DOMAIN_SURFACE | HISTORY_DOMAIN_LIGHTING);
    bool temporalAccepted = false;
    uint selectedAge = 0u;
    uint previousField = PtCheckerboardField(frameIndex - 1u);
    if (surface.valid && historyValid)
    {
        float4 motion = g_denoiseAov2[pixel];
        float2 jitterDelta =
            g_scene.jitterOptions.xy - g_scene.jitterOptions.zw;
        int2 reprojected = int2(round(
            float2(pixel) + motion.xy * float2(dimensions) + jitterDelta));
        if (all(reprojected >= 0) &&
            all(reprojected < int2(dimensions)))
        {
            uint2 previousReservoirPosition =
                RTXDI_PixelPosToReservoirPos(
                    uint2(reprojected),
                    previousField);
            uint2 previousPixel =
                RTXDI_ReservoirPosToPixelPos(
                    previousReservoirPosition,
                    previousField);
            if (all(previousPixel < dimensions) &&
                ValidatePreviousSurface(
                    pixel,
                    previousPixel,
                    surface.viewZ + motion.z))
            {
                RTXDI_PTReservoir previous =
                    RTXDI_UnpackPTReservoir(
                        g_restirPtHistory[
                            PtReservoirPointer(previousPixel, previousField)]);
                previous.M = min(
                    previous.M,
                    max(g_scene.restirOptions.w, 1.0f));
                previous.Age = min(
                    previous.Age + 1u,
                    (uint)RTXDI_PTRESERVOIR_AGE_MAX);
                if (previous.Age <= (uint)min(
                    max(round(g_scene.restirStabilityOptions.w), 1.0f),
                    (float)RTXDI_PTRESERVOIR_AGE_MAX))
                {
                    float3 target = PtTarget(surface, previous);
                    bool selected = CombineReservoirs(
                        result,
                        previous,
                        OwenScrambledSobol1D(
                            pixel,
                            frameIndex,
                            RtxdiDimensionGiTemporalCoin + 32u),
                        target);
                    if (selected)
                    {
                        selectedTarget = target;
                        selectedInitial = false;
                        selectedAge = previous.Age;
                    }
                    temporalAccepted =
                        temporalAccepted || Luminance(target) > 0.0f;
                }
            }
        }
    }

    uint spatialSamples = min(
        (uint)round(g_scene.restirOptions.y) * 2u,
        4u);
    if (surface.valid && historyValid && spatialSamples > 0u)
    {
        uint rotation = uint(OwenScrambledSobol1D(
            pixel,
            frameIndex,
            RtxdiDimensionGiSpatialRotation + 32u) * 16.0f) & 15u;
        float radiusScale =
            max(g_scene.restirOptions.z / 16.0f, 0.25f);
        [loop]
        for (uint sampleIndex = 0u;
            sampleIndex < spatialSamples;
            ++sampleIndex)
        {
            int2 neighborPixel = int2(pixel) + int2(round(
                float2(SpatialOffsets[(sampleIndex + rotation) & 15u]) *
                radiusScale));
            if (any(neighborPixel < 0) ||
                any(neighborPixel >= int2(dimensions)))
            {
                continue;
            }
            uint2 neighborReservoirPosition =
                RTXDI_PixelPosToReservoirPos(
                    uint2(neighborPixel),
                    previousField);
            uint2 activeNeighbor =
                RTXDI_ReservoirPosToPixelPos(
                    neighborReservoirPosition,
                    previousField);
            if (any(activeNeighbor >= dimensions) ||
                !ValidateSpatialSurface(pixel, activeNeighbor))
            {
                continue;
            }
            RTXDI_PTReservoir candidate = RTXDI_UnpackPTReservoir(
                g_restirPtHistory[
                    PtReservoirPointer(activeNeighbor, previousField)]);
            candidate.M = min(
                candidate.M,
                max(g_scene.restirOptions.w, 1.0f));
            float3 target = PtTarget(surface, candidate);
            bool selected = CombineReservoirs(
                result,
                candidate,
                OwenScrambledSobol1D(
                    pixel,
                    frameIndex,
                    RtxdiDimensionGiSpatialCoinBase + 32u + sampleIndex),
                target);
            if (selected)
            {
                selectedTarget = target;
                selectedInitial = false;
                selectedAge = candidate.Age;
            }
        }
    }

    result.M = min(
        result.M,
        max(g_scene.restirOptions.w, 1.0f));
    result.Age = selectedInitial ? 0u : selectedAge;
    float selectedLuminance = Luminance(selectedTarget);
    RTXDI_FinalizeResampling(
        result,
        1.0f,
        selectedLuminance * max(result.M, 1.0f));
    g_restirPtCurrent[PtReservoirPointer(pixel, currentField)] =
        RTXDI_PackPTReservoir(result);

    float3 diffuse = 0.0f.xxx;
    float3 specular = 0.0f.xxx;
    float hitDistance = 0.0f;
    if (surface.valid &&
        RTXDI_IsValidPTReservoir(result) &&
        result.WeightSum > 0.0f)
    {
        float3 sampleOffset =
            result.TranslatedWorldPosition - surface.worldPosition;
        hitDistance = length(sampleOffset);
        float3 direction = sampleOffset / max(hitDistance, 1.0e-8f);
        EvaluateBsdfLobes(
            surface,
            surface.viewDirection,
            direction,
            diffuse,
            specular);
        float3 scale =
            result.Radiance * result.WeightSum *
            PtFinalVisibility(pixel, surface, result);
        diffuse *= scale;
        specular *= scale;
    }
    float contributionLimit = max(g_scene.giOptions.y, 0.0f);
    float contributionLuminance = Luminance(diffuse + specular);
    if (contributionLimit > 0.0f &&
        contributionLuminance > contributionLimit)
    {
        float compression =
            contributionLimit / max(contributionLuminance, 1.0e-6f);
        diffuse *= compression;
        specular *= compression;
    }
    float4 diffuseSignal = g_signalDiffuse[pixel];
    float4 specularSignal = g_signalSpecular[pixel];
    AddSignalContribution(diffuseSignal, diffuse, hitDistance);
    AddSignalContribution(specularSignal, specular, hitDistance);
    g_signalDiffuse[pixel] = diffuseSignal;
    g_signalSpecular[pixel] = specularSignal;

    if ((uint)round(g_scene.debugOptions.x) == 0u &&
        g_scene.performanceOptions.w < 0.5f)
    {
        float3 hdr = max(
            g_postDenoiseHdr[pixel].rgb + diffuse + specular,
            0.0f.xxx);
        g_postDenoiseHdr[pixel] = float4(hdr, 1.0f);
        g_accumulation[pixel] = float4(hdr, 1.0f);
        g_output[pixel] = float4(Tonemap(hdr), 1.0f);
    }
}

#else

// Keep every build configuration shader-complete. These entry points are not
// dispatched when RTXDI is disabled or unavailable.
[numthreads(8, 8, 1)] void RtxdiDiCandidateCS(uint3 dispatchThreadId : SV_DispatchThreadID) { }
[numthreads(8, 8, 1)] void RtxdiDiTemporalCS(uint3 dispatchThreadId : SV_DispatchThreadID) { }
[numthreads(8, 8, 1)] void RtxdiDiSpatialCS(uint3 dispatchThreadId : SV_DispatchThreadID) { }
[numthreads(8, 8, 1)] void RtxdiDiShadeCS(uint3 dispatchThreadId : SV_DispatchThreadID) { }
[numthreads(8, 8, 1)] void RtxdiGiInitialCS(uint3 dispatchThreadId : SV_DispatchThreadID) { }
[numthreads(8, 8, 1)] void RtxdiGiFusedCS(uint3 dispatchThreadId : SV_DispatchThreadID) { }
[numthreads(8, 8, 1)] void RtxdiPtInitialCS(uint3 dispatchThreadId : SV_DispatchThreadID) { }
[numthreads(8, 8, 1)] void RtxdiPtFusedCS(uint3 dispatchThreadId : SV_DispatchThreadID) { }

#endif
