#if defined(VULKAN)
#define VK_BINDING(slot, descriptorSet) [[vk::binding(slot, descriptorSet)]]
#else
#define VK_BINDING(binding, set)
#endif

#ifndef PT_RESTIR
#define PT_RESTIR 0
#endif
#ifndef PT_RESTIR_DI
#define PT_RESTIR_DI 0
#endif
#ifndef PT_RESTIR_GI
#define PT_RESTIR_GI PT_RESTIR
#endif

static const float PI = 3.14159265359f;
static const uint TextureSlotBaseColor = 0;
static const uint TextureSlotNormal = 1;
static const uint TextureSlotRoughness = 2;
static const uint TextureSlotMetallic = 3;
static const uint TextureSlotOcclusion = 4;
static const uint TextureSlotEmissive = 5;
static const uint MaterialFeaturePackedOcclusionRoughnessMetallic = 1u << 0;
static const uint MaterialFeatureBaseColorTexture = 1u << 1;
static const uint MaterialFeatureNormalTexture = 1u << 2;
static const uint MaterialFeatureRoughnessTexture = 1u << 3;
static const uint MaterialFeatureMetallicTexture = 1u << 4;
static const uint MaterialFeatureOcclusionTexture = 1u << 5;
static const uint MaterialFeatureEmissiveTexture = 1u << 6;

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

struct RayPayload
{
    // DXR path payload ABI: 32 bytes. Keep this limited to intersection data
    // that only the traversal shaders can produce. Material evaluation stays
    // in RayGen so texture/BSDF state is not carried through TraceRay.
    float2 barycentrics;
    float hitT;
    float rayConeWidth;
    float rayConeSpread;
    uint instanceIndex;
    uint geometryIndex;
    uint primitiveIndex;
};

struct ShadowPayload
{
    uint occluded;
    float rayConeWidth;
    float rayConeSpread;
};

struct SurfaceData
{
    float3 position;
    float3 normal;
    float4 tangent;
    float2 texcoord;
    float3 baseColor;
    float3 normalTexture;
    float ao;
    float roughness;
    float metallic;
    float3 emissive;
    RtMaterial material;
    uint materialIndex;
    float rayConeWidth;
    float rayConeSpread;
    float coverage;
};

// Shaded first-hit data is a RayGen-local value, not a DXR payload. Keeping it
// separate preserves all debug/AOV guides without inflating every TraceRay.
struct PathHitData
{
    SurfaceData surface;
    float hitT;
    uint hit;
    uint instanceIndex;
    uint geometryIndex;
    uint primitiveIndex;
};

struct RtLight
{
    float4 positionArea;
    float4 edge0Type;
    float4 edge1;
    float4 radianceCdf;
    uint4 meshIdentity;
};

VK_BINDING(0, 0) RaytracingAccelerationStructure g_sceneAs : register(t0, space0);
VK_BINDING(1, 0) RWTexture2D<float4> g_output : register(u0, space0);
VK_BINDING(2, 0) RWTexture2D<float4> g_accumulation : register(u1, space0);
VK_BINDING(3, 0) ConstantBuffer<SceneConstants> g_scene : register(b0, space0);
VK_BINDING(4, 0) StructuredBuffer<MeshVertex> g_vertices : register(t1, space0);
VK_BINDING(5, 0) StructuredBuffer<uint> g_indices : register(t2, space0);
VK_BINDING(6, 0) StructuredBuffer<RtGeometryRecord> g_geometries : register(t3, space0);
VK_BINDING(7, 0) StructuredBuffer<RtMaterial> g_materials : register(t4, space0);
VK_BINDING(8, 0) SamplerState g_linearSampler : register(s0, space0);
VK_BINDING(12, 0) StructuredBuffer<RtLight> g_lights : register(t5, space0);
VK_BINDING(13, 0) RWTexture2D<float4> g_denoiseAov0 : register(u5, space0);
VK_BINDING(14, 0) RWTexture2D<float4> g_denoiseAov1 : register(u6, space0);
VK_BINDING(15, 0) RWTexture2D<float4> g_denoiseAov2 : register(u7, space0);
VK_BINDING(16, 0) RWTexture2D<float4> g_reconstructionHistoryRadiance : register(u8, space0);
VK_BINDING(17, 0) RWTexture2D<float4> g_reconstructionHistoryMoments : register(u9, space0);
VK_BINDING(18, 0) RWTexture2D<float4> g_reconstructionHistoryLength : register(u10, space0);
VK_BINDING(19, 0) RWTexture2D<float4> g_previousDenoiseAov0 : register(u11, space0);
VK_BINDING(20, 0) RWTexture2D<float4> g_previousDenoiseAov1 : register(u12, space0);
VK_BINDING(21, 0) RWTexture2D<float4> g_previousDenoiseAov2 : register(u13, space0);
VK_BINDING(25, 0) RWTexture2D<float4> g_signalCurrentRadiance : register(u17, space0);
VK_BINDING(26, 0) RWTexture2D<float4> g_signalDirect : register(u18, space0);
VK_BINDING(27, 0) RWTexture2D<float4> g_signalIndirect : register(u19, space0);
VK_BINDING(28, 0) RWTexture2D<float4> g_signalResidual : register(u20, space0);
VK_BINDING(29, 0) RWTexture2D<float4> g_denoisePing : register(u21, space0);
VK_BINDING(30, 0) RWTexture2D<float4> g_denoisePong : register(u22, space0);
VK_BINDING(38, 0) RWTexture2D<float4> g_reconstructionHistoryRadianceB : register(u30, space0);
VK_BINDING(39, 0) RWTexture2D<float4> g_reconstructionHistoryMomentsB : register(u31, space0);
VK_BINDING(40, 0) RWTexture2D<float4> g_reconstructionHistoryLengthB : register(u32, space0);
VK_BINDING(41, 0) RWTexture2D<float4> g_postDenoiseHdr : register(u33, space0);
VK_BINDING(42, 0) RWTexture2D<float4> g_taaHistoryA : register(u34, space0);
VK_BINDING(43, 0) RWTexture2D<float4> g_taaHistoryB : register(u35, space0);
VK_BINDING(44, 0) RWTexture2D<float> g_diffuseHistoryConfidence : register(u36, space0);
VK_BINDING(45, 0) RWTexture2D<float> g_specularHistoryConfidence : register(u37, space0);
VK_BINDING(46, 0) RWTexture2D<uint> g_surfaceIdentity : register(u38, space0);
VK_BINDING(47, 0) RWTexture2D<uint> g_previousSurfaceIdentity : register(u39, space0);
#include "PathTracingQualityCounters.hlsli"
VK_BINDING(0, 1) Texture2D g_textures[] : register(t0, space1);

bool PreviousHistoryIsA()
{
    return (g_scene.frameOptions.w & 1u) == 0u;
}

static const float HistoryLengthUnormScale = 255.0f;

float4 UnpackHistoryControls(float4 packedControls)
{
    return float4(
        round(saturate(packedControls.xy) * HistoryLengthUnormScale),
        saturate(packedControls.zw));
}

float4 LoadPreviousHistoryMoments(uint2 pixel)
{
    return PreviousHistoryIsA() ? g_reconstructionHistoryMoments[pixel] : g_reconstructionHistoryMomentsB[pixel];
}

float4 LoadPreviousHistoryLength(uint2 pixel)
{
    float4 packedControls = PreviousHistoryIsA()
        ? g_reconstructionHistoryLength[pixel]
        : g_reconstructionHistoryLengthB[pixel];
    return UnpackHistoryControls(packedControls);
}

#include "PathTracingSampling.hlsli"

uint PackSurfaceIdentity(
    uint instanceIndex,
    uint geometryIndex,
    uint primitiveIndex,
    uint materialIndex,
    float coverage)
{
    uint groupIdentity = Hash(
        instanceIndex ^
        Hash(geometryIndex + 0x9e3779b9u) ^
        Hash(materialIndex + 0x85ebca6bu));
    groupIdentity = max(groupIdentity & ((1u << 22u) - 1u), 1u);
    uint primitiveSignature = Hash(primitiveIndex + 0xc2b2ae35u) & SURFACE_IDENTITY_PRIMITIVE_MASK;
    uint quantizedCoverage = (uint)round(saturate(coverage) * (float)SURFACE_IDENTITY_COVERAGE_MASK);
    return (groupIdentity << SURFACE_IDENTITY_GROUP_SHIFT) |
        (primitiveSignature << SURFACE_IDENTITY_COVERAGE_BITS) |
        quantizedCoverage;
}

float3 AcesTonemap(float3 value)
{
    const float a = 2.51f;
    const float b = 0.03f;
    const float c = 2.43f;
    const float d = 0.59f;
    const float e = 0.14f;
    return saturate((value * (a * value + b)) / (value * (c * value + d) + e));
}

float3 Tonemap(float3 value)
{
    value = max(value * exp2(g_scene.viewOptions.x), 0.0f.xxx);
    uint toneMapper = (uint)round(g_scene.viewOptions.z);
    if (toneMapper == 1u)
    {
        value = value / (1.0f.xxx + value);
    }
    else if (toneMapper == 2u)
    {
        value = AcesTonemap(value);
    }
    float gamma = max(g_scene.viewOptions.y, 0.01f);
    return pow(saturate(value), 1.0f / gamma);
}

float3 ApplyMaterialFocus(float3 value, uint materialIndex, uint hit)
{
    uint focusMode = (uint)round(g_scene.materialFocusOptions.x);
    if (focusMode == 0u || hit == 0u)
    {
        return value;
    }
    uint selectedMaterial = (uint)round(g_scene.materialFocusOptions.y);
    if (materialIndex == selectedMaterial)
    {
        return value;
    }
    if (focusMode == 1u)
    {
        return 0.0f.xxx;
    }
    return value * 0.16f;
}

float Luminance(float3 color)
{
    return dot(color, float3(0.2126f, 0.7152f, 0.0722f));
}

float MaxComponent(float3 value)
{
    return max(value.x, max(value.y, value.z));
}

uint PixelIndex(uint2 pixel, uint2 dimensions)
{
    return pixel.y * dimensions.x + pixel.x;
}

float2 EnvironmentUv(float3 rayDirection)
{
    float phi = atan2(rayDirection.x, rayDirection.z) + g_scene.environmentOptions.z;
    float u = frac(phi / (2.0f * PI) + 0.5f);
    float v = acos(clamp(rayDirection.y, -1.0f, 1.0f)) / PI;
    return float2(u, v);
}

float EnvironmentTexelSolidAngle(uint row, uint width, uint height)
{
    float theta0 = PI * (float)row / (float)height;
    float theta1 = PI * (float)(row + 1u) / (float)height;
    return (2.0f * PI / (float)width) * max(cos(theta0) - cos(theta1), 1.0e-10f);
}

float EnvironmentDirectionPdf(float3 rayDirection)
{
    uint environmentTextureIndex = (uint)round(g_scene.lightOptions.w);
    uint aliasTextureIndex = NonUniformResourceIndex(environmentTextureIndex + 1u);
    uint width;
    uint height;
    g_textures[aliasTextureIndex].GetDimensions(width, height);
    float2 uv = EnvironmentUv(rayDirection);
    uint2 texel = min((uint2)(uv * float2(width, height)), uint2(width - 1u, height - 1u));
    float texelProbability = g_textures[aliasTextureIndex].Load(int3(texel, 0)).z;
    return texelProbability / EnvironmentTexelSolidAngle(texel.y, width, height);
}

void SampleEnvironmentDirection(float4 sample, out float3 direction, out float directionPdf)
{
    uint environmentTextureIndex = (uint)round(g_scene.lightOptions.w);
    uint aliasTextureIndex = NonUniformResourceIndex(environmentTextureIndex + 1u);
    uint width;
    uint height;
    g_textures[aliasTextureIndex].GetDimensions(width, height);
    uint entryCount = max(width * height, 1u);
    uint bucket = min((uint)(sample.x * (float)entryCount), entryCount - 1u);
    uint2 bucketTexel = uint2(bucket % width, bucket / width);
    float4 aliasEntry = g_textures[aliasTextureIndex].Load(int3(bucketTexel, 0));
    uint selected = sample.y < saturate(aliasEntry.x)
        ? bucket
        : min((uint)round(aliasEntry.y), entryCount - 1u);
    uint2 selectedTexel = uint2(selected % width, selected / width);
    float u = ((float)selectedTexel.x + sample.z) / (float)width;
    float theta0 = PI * (float)selectedTexel.y / (float)height;
    float theta1 = PI * (float)(selectedTexel.y + 1u) / (float)height;
    // Sample uniformly in solid angle inside the selected lat-long texel.
    // Uniform theta would not match p(texel) / solidAngle(texel), especially
    // in the polar rows, and would bias light/BSDF MIS.
    float cosTheta = lerp(cos(theta0), cos(theta1), sample.w);
    float sinTheta = sqrt(saturate(1.0f - cosTheta * cosTheta));
    float phi = 2.0f * PI * (u - 0.5f) - g_scene.environmentOptions.z;
    direction = float3(sinTheta * sin(phi), cosTheta, sinTheta * cos(phi));
    float texelProbability = g_textures[aliasTextureIndex].Load(int3(selectedTexel, 0)).z;
    directionPdf = texelProbability / EnvironmentTexelSolidAngle(selectedTexel.y, width, height);
}

float SkyNeePdf(float3 surfaceNormal, float3 direction)
{
    return g_scene.environmentOptions.x > 0.5f
        ? EnvironmentDirectionPdf(direction)
        : saturate(dot(surfaceNormal, direction)) / PI;
}

float TextureMipLevel(uint textureIndex, float uvFootprint)
{
    uint resourceIndex = NonUniformResourceIndex(textureIndex);
    uint width;
    uint height;
    uint mipLevels;
    g_textures[resourceIndex].GetDimensions(0u, width, height, mipLevels);
    float texelFootprint = max(max(uvFootprint, 0.0f) * (float)max(width, height), 1.0f);
    return clamp(log2(texelFootprint), 0.0f, (float)max(mipLevels, 1u) - 1.0f);
}

float4 SampleTextureWithFootprint(uint textureIndex, float2 uv, float uvFootprint)
{
    uint resourceIndex = NonUniformResourceIndex(textureIndex);
    float mipLevel = TextureMipLevel(textureIndex, uvFootprint);
    return g_textures[resourceIndex].SampleLevel(g_linearSampler, uv, mipLevel);
}

float EnvironmentMipLevel(uint textureIndex, float3 rayDirection, float coneSpread)
{
    uint resourceIndex = NonUniformResourceIndex(textureIndex);
    uint width;
    uint height;
    uint mipLevels;
    g_textures[resourceIndex].GetDimensions(0u, width, height, mipLevels);

    float angularRadius = max(coneSpread, 0.0f);
    float sinTheta = max(sqrt(saturate(1.0f - rayDirection.y * rayDirection.y)), 0.1f);
    float footprintU = angularRadius * (float)width / (2.0f * PI * sinTheta);
    float footprintV = angularRadius * (float)height / PI;
    float texelFootprint = max(max(footprintU, footprintV), 1.0f);
    return clamp(log2(texelFootprint), 0.0f, (float)max(mipLevels, 1u) - 1.0f);
}

float3 EvaluateEnvironmentMap(float3 rayDirection, float coneSpread)
{
    uint environmentTextureIndex = (uint)round(g_scene.lightOptions.w);
    uint resourceIndex = NonUniformResourceIndex(environmentTextureIndex);
    float mipLevel = EnvironmentMipLevel(environmentTextureIndex, rayDirection, coneSpread);
    return g_textures[resourceIndex].SampleLevel(g_linearSampler, EnvironmentUv(rayDirection), mipLevel).rgb * g_scene.environmentOptions.y;
}

float3 EvaluateEnvironmentMap(float3 rayDirection)
{
    return EvaluateEnvironmentMap(rayDirection, 0.0f);
}

float3 EvaluateSky(float3 rayDirection, float coneSpread)
{
    if (g_scene.environmentOptions.x > 0.5f)
    {
        return EvaluateEnvironmentMap(rayDirection, coneSpread);
    }

    float3 fallback = g_scene.skyColor.rgb * g_scene.skyColor.a;
    if (g_scene.skyOptions.w < 0.5f)
    {
        return fallback;
    }

    float y = rayDirection.y;
    float horizonBlend = saturate(y * 0.5f + 0.5f);
    float3 sky = lerp(g_scene.skyGroundColor.rgb, g_scene.skyHorizonColor.rgb, smoothstep(-g_scene.skyOptions.z, 0.15f, y));
    sky = lerp(sky, g_scene.skyZenithColor.rgb, horizonBlend * horizonBlend);

    float3 sunDirection = normalize(-g_scene.lightDirection.xyz);
    float sunDot = saturate(dot(rayDirection, sunDirection));
    float sunSize = max(g_scene.skyOptions.y, 0.001f);
    float sunDisk = smoothstep(cos(sunSize * 2.0f), cos(sunSize), sunDot);
    float3 sun = g_scene.lightColor.rgb * g_scene.skyOptions.x * sunDisk;
    return (sky * g_scene.skyColor.a) + sun + fallback * 0.05f;
}

float3 EvaluateSky(float3 rayDirection)
{
    return EvaluateSky(rayDirection, 0.0f);
}

float3 ClampRadiance(float3 radiance)
{
    float limit = max(g_scene.giOptions.y, 0.0f);
    if (limit <= 0.0f)
    {
        return radiance;
    }
    float lum = Luminance(radiance);
    if (lum > limit)
    {
        radiance *= limit / max(lum, 0.0001f);
    }
    return radiance;
}

float3 ApplyTemporalClamp(float3 current, float3 history, uint accumulatedFrames)
{
    // A zero contribution limit is the unbiased reference-mode contract: no
    // per-frame radiance compression and no temporal neighborhood clamp.
    if (accumulatedFrames == 0u || g_scene.giOptions.y <= 0.0f)
    {
        return current;
    }
    float clampScale = max(g_scene.giOptions.z, 0.0f);
    float clampMin = max(g_scene.giOptions.w, 0.0f);
    float3 delta = max(abs(history) * clampScale, clampMin.xxx);
    return clamp(current, history - delta, history + delta);
}

float3 Barycentric3(float2 barycentrics)
{
    return float3(1.0f - barycentrics.x - barycentrics.y, barycentrics.x, barycentrics.y);
}

float3 Interpolate3(float3 a, float3 b, float3 c, float3 bary)
{
    return a * bary.x + b * bary.y + c * bary.z;
}

float2 Interpolate2(float2 a, float2 b, float2 c, float3 bary)
{
    return a * bary.x + b * bary.y + c * bary.z;
}

float4 Interpolate4(float4 a, float4 b, float4 c, float3 bary)
{
    return a * bary.x + b * bary.y + c * bary.z;
}

uint3 LoadTriangleIndices(RtGeometryRecord geometry, uint primitiveIndex)
{
    uint index = geometry.indexOffset + primitiveIndex * 3u;
    return uint3(g_indices[index + 0], g_indices[index + 1], g_indices[index + 2]);
}

float RayConeWidthAtDistance(float coneWidth, float coneSpread, float distance)
{
    return max(coneWidth + max(coneSpread, 0.0f) * max(distance, 0.0f), 0.0f);
}

float TriangleUvFootprint(uint geometryIndex, uint primitiveIndex, float coneWidthAtHit, float3 rayDirection)
{
    RtGeometryRecord geometry = g_geometries[geometryIndex];
    uint3 tri = LoadTriangleIndices(geometry, primitiveIndex);
    MeshVertex v0 = g_vertices[tri.x];
    MeshVertex v1 = g_vertices[tri.y];
    MeshVertex v2 = g_vertices[tri.z];

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
    float uvPerWorld = sqrt(uvArea2 / worldArea2);
    return min(max(coneWidthAtHit, 0.0f) * uvPerWorld / incidence, 4.0f);
}

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
        // Match the existing black fallback texture without issuing a texture
        // read or a mip-footprint query.
        return 0.0f.xxx;
    }

    uint geometryIndex = light.meshIdentity.x;
    uint primitiveIndex = light.meshIdentity.y;
    RtGeometryRecord geometry = g_geometries[geometryIndex];
    uint3 tri = LoadTriangleIndices(geometry, primitiveIndex);
    float2 texcoord = Interpolate2(
        g_vertices[tri.x].texcoord,
        g_vertices[tri.y].texcoord,
        g_vertices[tri.z].texcoord,
        barycentrics);
    float uvFootprint = TriangleUvFootprint(
        geometryIndex,
        primitiveIndex,
        coneWidthAtHit,
        rayDirection);
    float3 emissiveTexture = SampleTextureWithFootprint(
        material.textureBaseIndex + TextureSlotEmissive,
        texcoord,
        uvFootprint).rgb;
    return emissiveTexture * material.emissiveFactor.rgb *
        material.emissiveFactor.a * g_scene.lightOptions.y;
}

SurfaceData LoadSurface(
    uint geometryIndex,
    uint primitiveIndex,
    float2 barycentrics,
    float uvFootprint,
    float coneWidthAtHit,
    float coneSpread)
{
    RtGeometryRecord geometry = g_geometries[geometryIndex];
    uint3 tri = LoadTriangleIndices(geometry, primitiveIndex);
    MeshVertex v0 = g_vertices[tri.x];
    MeshVertex v1 = g_vertices[tri.y];
    MeshVertex v2 = g_vertices[tri.z];
    float3 bary = Barycentric3(barycentrics);

    SurfaceData surface;
    surface.position = Interpolate3(v0.position, v1.position, v2.position, bary);
    surface.normal = normalize(Interpolate3(v0.normal, v1.normal, v2.normal, bary));
    surface.tangent = Interpolate4(v0.tangent, v1.tangent, v2.tangent, bary);
    surface.texcoord = Interpolate2(v0.texcoord, v1.texcoord, v2.texcoord, bary);
    surface.material = g_materials[geometry.materialIndex];
    surface.materialIndex = geometry.materialIndex;
    surface.rayConeWidth = coneWidthAtHit;
    surface.rayConeSpread = coneSpread;

    // These constants reproduce the byte-valued fallback textures created by
    // CreateTextures, keeping the material ABI and output unchanged while
    // avoiding descriptor and texture accesses for absent maps.
    const float defaultRoughnessTexel = 122.0f / 255.0f;
    float4 baseSample = 1.0f.xxxx;
    if (HasMaterialFeature(surface.material, MaterialFeatureBaseColorTexture))
    {
        baseSample = SampleTextureWithFootprint(
            surface.material.textureBaseIndex + TextureSlotBaseColor,
            surface.texcoord,
            uvFootprint);
    }
    float3 normalTexture = float3(128.0f / 255.0f, 128.0f / 255.0f, 1.0f);
    if (HasMaterialFeature(surface.material, MaterialFeatureNormalTexture))
    {
        normalTexture = SampleTextureWithFootprint(
            surface.material.textureBaseIndex + TextureSlotNormal,
            surface.texcoord,
            uvFootprint).xyz;
    }
    surface.baseColor = baseSample.rgb * surface.material.baseColorFactor.rgb;
    surface.coverage = surface.material.alphaMasked != 0u
        ? saturate(baseSample.a * surface.material.baseColorFactor.a)
        : 1.0f;
    if (HasMaterialFeature(surface.material, MaterialFeaturePackedOcclusionRoughnessMetallic))
    {
        float3 ormSample = defaultRoughnessTexel.xxx;
        if (HasMaterialFeature(surface.material, MaterialFeatureRoughnessTexture))
        {
            ormSample = SampleTextureWithFootprint(
                surface.material.textureBaseIndex + TextureSlotRoughness,
                surface.texcoord,
                uvFootprint).rgb;
        }
        surface.ao = saturate(ormSample.r * surface.material.occlusionStrength);
        surface.roughness = clamp(ormSample.g * surface.material.roughnessFactor, 0.04f, 1.0f);
        surface.metallic = saturate(ormSample.b * surface.material.metallicFactor);
    }
    else
    {
        float roughnessTexel = defaultRoughnessTexel;
        if (HasMaterialFeature(surface.material, MaterialFeatureRoughnessTexture))
        {
            roughnessTexel = SampleTextureWithFootprint(
                surface.material.textureBaseIndex + TextureSlotRoughness,
                surface.texcoord,
                uvFootprint).r;
        }
        float metallicTexel = 0.0f;
        if (HasMaterialFeature(surface.material, MaterialFeatureMetallicTexture))
        {
            metallicTexel = SampleTextureWithFootprint(
                surface.material.textureBaseIndex + TextureSlotMetallic,
                surface.texcoord,
                uvFootprint).r;
        }
        float occlusionTexel = 1.0f;
        if (HasMaterialFeature(surface.material, MaterialFeatureOcclusionTexture))
        {
            occlusionTexel = SampleTextureWithFootprint(
                surface.material.textureBaseIndex + TextureSlotOcclusion,
                surface.texcoord,
                uvFootprint).r;
        }
        surface.ao = saturate(occlusionTexel * surface.material.occlusionStrength);
        surface.roughness = clamp(roughnessTexel * surface.material.roughnessFactor, 0.04f, 1.0f);
        surface.metallic = saturate(metallicTexel * surface.material.metallicFactor);
    }
    float3 emissiveTexture = 0.0f.xxx;
    if (HasMaterialFeature(surface.material, MaterialFeatureEmissiveTexture))
    {
        emissiveTexture = SampleTextureWithFootprint(
            surface.material.textureBaseIndex + TextureSlotEmissive,
            surface.texcoord,
            uvFootprint).rgb;
    }
    surface.emissive = emissiveTexture * surface.material.emissiveFactor.rgb * surface.material.emissiveFactor.a * g_scene.lightOptions.y;
    surface.normalTexture = normalTexture;

    float3 decodedNormal = normalTexture * 2.0f - 1.0f;
    float averagedNormalLength = saturate(length(decodedNormal));
    float normalVariance = saturate(1.0f - averagedNormalLength) * surface.material.normalStrength * surface.material.normalStrength;
    surface.roughness = clamp(sqrt(surface.roughness * surface.roughness + normalVariance), 0.04f, 1.0f);

    float2 normalXY = (normalTexture.xy * 2.0f - 1.0f) * surface.material.normalStrength;
    normalXY.y *= g_scene.debugOptions.y > 0.5f ? -1.0f : 1.0f;
    float3 normalSample = float3(normalXY, sqrt(saturate(1.0f - dot(normalXY, normalXY))));
    float3 n = surface.normal;
    float3 t = normalize(surface.tangent.xyz - n * dot(n, surface.tangent.xyz));
    float3 b = normalize(cross(n, t) * surface.tangent.w);
    surface.normal = normalize(normalSample.x * t + normalSample.y * b + normalSample.z * n);
    return surface;
}

float AlphaCoverageCutoff(float baseCutoff, float mipLevel)
{
    // Alpha-masked base-color textures now carry coverage-preserving mip data,
    // so the material threshold stays invariant across ray-cone LODs.
    return baseCutoff;
}

bool IsAlphaTransparent(
    uint geometryIndex,
    uint primitiveIndex,
    float2 barycentrics,
    float coneWidth,
    float coneSpread,
    float hitDistance,
    float3 rayDirection)
{
    RtGeometryRecord geometry = g_geometries[geometryIndex];
    RtMaterial material = g_materials[geometry.materialIndex];
    if (material.alphaMasked == 0)
    {
        return false;
    }
    if (!HasMaterialFeature(material, MaterialFeatureBaseColorTexture))
    {
        return material.baseColorFactor.a < AlphaCoverageCutoff(material.alphaCutoff, 0.0f);
    }

    uint3 tri = LoadTriangleIndices(geometry, primitiveIndex);
    MeshVertex v0 = g_vertices[tri.x];
    MeshVertex v1 = g_vertices[tri.y];
    MeshVertex v2 = g_vertices[tri.z];
    float3 bary = Barycentric3(barycentrics);
    float2 texcoord = Interpolate2(v0.texcoord, v1.texcoord, v2.texcoord, bary);
    float coneWidthAtHit = RayConeWidthAtDistance(coneWidth, coneSpread, hitDistance);
    float uvFootprint = TriangleUvFootprint(geometryIndex, primitiveIndex, coneWidthAtHit, rayDirection);
    uint textureIndex = material.textureBaseIndex + TextureSlotBaseColor;
    uint resourceIndex = NonUniformResourceIndex(textureIndex);
    float mipLevel = TextureMipLevel(textureIndex, uvFootprint);
    float alpha = material.baseColorFactor.a *
        g_textures[resourceIndex].SampleLevel(g_linearSampler, texcoord, mipLevel).a;
    return alpha < AlphaCoverageCutoff(material.alphaCutoff, mipLevel);
}

void BuildBasis(float3 normal, out float3 tangent, out float3 bitangent)
{
    float3 up = abs(normal.z) < 0.999f ? float3(0.0f, 0.0f, 1.0f) : float3(1.0f, 0.0f, 0.0f);
    tangent = normalize(cross(up, normal));
    bitangent = cross(normal, tangent);
}

float3 TangentToWorld(float3 localDirection, float3 normal)
{
    float3 tangent;
    float3 bitangent;
    BuildBasis(normal, tangent, bitangent);
    return normalize(localDirection.x * tangent + localDirection.y * bitangent + localDirection.z * normal);
}

float3 SampleCosineHemisphere(float2 sample, float3 normal)
{
    float phi = 2.0f * PI * sample.x;
    float r = sqrt(sample.y);
    float x = r * cos(phi);
    float y = r * sin(phi);
    float z = sqrt(max(0.0f, 1.0f - sample.y));
    return TangentToWorld(float3(x, y, z), normal);
}

float3 SampleGGXDirection(float2 sample, float roughness, float3 normal, float3 viewDirection)
{
    float3 tangent;
    float3 bitangent;
    BuildBasis(normal, tangent, bitangent);
    float3 viewLocal = float3(dot(viewDirection, tangent), dot(viewDirection, bitangent), dot(viewDirection, normal));
    viewLocal.z = max(viewLocal.z, 1.0e-5f);

    // Heitz isotropic GGX VNDF sampling: stretch the view, sample the visible
    // projected disk, then unstretch the microfacet normal.
    float alpha = max(roughness * roughness, 0.001f);
    float3 stretchedView = normalize(float3(alpha * viewLocal.x, alpha * viewLocal.y, viewLocal.z));
    float lensq = stretchedView.x * stretchedView.x + stretchedView.y * stretchedView.y;
    float3 basisX = lensq > 1.0e-8f
        ? float3(-stretchedView.y, stretchedView.x, 0.0f) * rsqrt(lensq)
        : float3(1.0f, 0.0f, 0.0f);
    float3 basisY = cross(stretchedView, basisX);

    float radius = sqrt(sample.x);
    float phi = 2.0f * PI * sample.y;
    float diskX = radius * cos(phi);
    float diskY = radius * sin(phi);
    float viewBlend = 0.5f * (1.0f + stretchedView.z);
    diskY = lerp(sqrt(max(0.0f, 1.0f - diskX * diskX)), diskY, viewBlend);
    float diskZ = sqrt(max(0.0f, 1.0f - diskX * diskX - diskY * diskY));
    float3 stretchedNormal = diskX * basisX + diskY * basisY + diskZ * stretchedView;
    float3 halfLocal = normalize(float3(alpha * stretchedNormal.x, alpha * stretchedNormal.y, max(stretchedNormal.z, 0.0f)));
    float3 halfVector = normalize(halfLocal.x * tangent + halfLocal.y * bitangent + halfLocal.z * normal);
    return normalize(reflect(-viewDirection, halfVector));
}

float3 FresnelSchlick(float cosTheta, float3 f0)
{
    return f0 + (1.0f - saturate(f0)) * pow(1.0f - saturate(cosTheta), 5.0f);
}

float DistributionGGX(float3 normal, float3 halfVector, float roughness)
{
    float alpha = max(roughness * roughness, 0.001f);
    float alpha2 = alpha * alpha;
    float nDotH = saturate(dot(normal, halfVector));
    float nDotH2 = nDotH * nDotH;
    float denominator = nDotH2 * (alpha2 - 1.0f) + 1.0f;
    return alpha2 / max(PI * denominator * denominator, 1.0e-8f);
}

float SmithGGXLambda(float nDotDirection, float roughness)
{
    float cosine = max(nDotDirection, 1.0e-6f);
    float cosine2 = cosine * cosine;
    float alpha = max(roughness * roughness, 0.001f);
    float tan2 = max(1.0f - cosine2, 0.0f) / cosine2;
    return 0.5f * (sqrt(1.0f + alpha * alpha * tan2) - 1.0f);
}

float SmithGGXG1(float nDotDirection, float roughness)
{
    return nDotDirection > 0.0f ? 1.0f / (1.0f + SmithGGXLambda(nDotDirection, roughness)) : 0.0f;
}

float GeometrySmith(float3 normal, float3 viewDirection, float3 lightDirection, float roughness)
{
    float nDotV = saturate(dot(normal, viewDirection));
    float nDotL = saturate(dot(normal, lightDirection));
    if (nDotV <= 0.0f || nDotL <= 0.0f)
    {
        return 0.0f;
    }
    return 1.0f / (1.0f + SmithGGXLambda(nDotV, roughness) + SmithGGXLambda(nDotL, roughness));
}

float BsdfSpecularProbability(SurfaceData surface, float3 viewDirection)
{
    float3 f0 = lerp(0.04f.xxx, surface.baseColor, surface.metallic);
    float fresnelWeight = saturate(MaxComponent(FresnelSchlick(saturate(dot(surface.normal, viewDirection)), f0)));
    float probability = 0.2f + 0.55f * (1.0f - surface.roughness) + 0.25f * surface.metallic + 0.25f * fresnelWeight;
    return clamp(probability, 0.05f, 0.95f);
}

float CosineHemispherePdf(float3 normal, float3 direction)
{
    return saturate(dot(normal, direction)) / PI;
}

float GGXReflectionPdf(SurfaceData surface, float3 viewDirection, float3 lightDirection)
{
    float nDotV = saturate(dot(surface.normal, viewDirection));
    float nDotL = saturate(dot(surface.normal, lightDirection));
    if (nDotV <= 0.0f || nDotL <= 0.0f)
    {
        return 0.0f;
    }

    float3 halfSum = viewDirection + lightDirection;
    float halfLength2 = dot(halfSum, halfSum);
    if (halfLength2 <= 1.0e-10f)
    {
        return 0.0f;
    }
    float3 halfVector = halfSum * rsqrt(halfLength2);
    float vDotH = abs(dot(viewDirection, halfVector));
    float lDotH = abs(dot(lightDirection, halfVector));
    if (vDotH <= 1.0e-7f || lDotH <= 1.0e-7f)
    {
        return 0.0f;
    }

    float visibleNormalPdf = DistributionGGX(surface.normal, halfVector, surface.roughness)
        * SmithGGXG1(nDotV, surface.roughness) * vDotH / max(nDotV, 1.0e-7f);
    return visibleNormalPdf / max(4.0f * lDotH, 1.0e-7f);
}

float EvaluateBsdfPdf(SurfaceData surface, float3 viewDirection, float3 lightDirection)
{
    float specularProbability = BsdfSpecularProbability(surface, viewDirection);
    float diffusePdf = CosineHemispherePdf(surface.normal, lightDirection);
    float specularPdf = GGXReflectionPdf(surface, viewDirection, lightDirection);
    return (1.0f - specularProbability) * diffusePdf + specularProbability * specularPdf;
}

float PowerHeuristic(float pdfA, float pdfB)
{
    float a2 = pdfA * pdfA;
    float b2 = pdfB * pdfB;
    return a2 / max(a2 + b2, 1.0e-20f);
}

void EvaluateBsdfLobes(
    SurfaceData surface,
    float3 viewDirection,
    float3 lightDirection,
    out float3 diffuse,
    out float3 specular)
{
    diffuse = 0.0f.xxx;
    specular = 0.0f.xxx;
    float nDotL = saturate(dot(surface.normal, lightDirection));
    if (nDotL <= 0.0f)
    {
        return;
    }

    float3 halfSum = viewDirection + lightDirection;
    if (dot(halfSum, halfSum) <= 1.0e-10f)
    {
        return;
    }
    float3 halfVector = normalize(halfSum);
    float3 f0 = lerp(0.04f.xxx, surface.baseColor, surface.metallic);
    float3 fresnel = FresnelSchlick(saturate(dot(halfVector, viewDirection)), f0);
    float distribution = DistributionGGX(surface.normal, halfVector, surface.roughness);
    float geometry = GeometrySmith(surface.normal, viewDirection, lightDirection, surface.roughness);
    float nDotV = saturate(dot(surface.normal, viewDirection));
    specular = distribution * geometry * fresnel / max(4.0f * nDotV * nDotL, 0.0001f) * nDotL;
    diffuse = (1.0f.xxx - fresnel) * (1.0f - surface.metallic) * surface.baseColor / PI * nDotL;
}

float3 EvaluateBsdf(SurfaceData surface, float3 viewDirection, float3 lightDirection)
{
    float3 diffuse;
    float3 specular;
    EvaluateBsdfLobes(surface, viewDirection, lightDirection, diffuse, specular);
    return diffuse + specular;
}

float TraceVisibilityRay(
    float3 origin,
    float3 normal,
    float3 direction,
    float tMax,
    float coneWidth,
    float coneSpread)
{
    ShadowPayload payload;
    payload.occluded = 1;
    payload.rayConeWidth = coneWidth;
    payload.rayConeSpread = coneSpread;

    RayDesc ray;
    ray.Origin = origin + normal * g_scene.rayOptions.x;
    ray.Direction = direction;
    ray.TMin = g_scene.rayOptions.x;
    ray.TMax = tMax;
    TraceRay(g_sceneAs, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER, 0xff, 1, 0, 1, ray, payload);
    return payload.occluded == 0 ? 1.0f : 0.0f;
}

float3 EvaluateSunNEE(
    SurfaceData surface,
    float3 viewDirection,
    out float3 diffuseContribution,
    out float3 specularContribution)
{
    diffuseContribution = 0.0f.xxx;
    specularContribution = 0.0f.xxx;
    if (g_scene.debugOptions.z < 0.5f)
    {
        return 0.0f.xxx;
    }

    // The host intensity is directional irradiance, so the sun is retained as
    // a delta-direction light (discrete PDF 1, MIS weight 1). The separately
    // shaded procedural sky disk remains part of the environment strategy.
    float3 lightDirection = normalize(-g_scene.lightDirection.xyz);
    float nDotL = dot(surface.normal, lightDirection);
    if (nDotL <= 0.0f)
    {
        return 0.0f.xxx;
    }

    float visibility = TraceVisibilityRay(
        surface.position,
        surface.normal,
        lightDirection,
        g_scene.rayOptions.y,
        surface.rayConeWidth,
        surface.rayConeSpread);
    if (visibility <= 0.0f)
    {
        return 0.0f.xxx;
    }

    float3 radiance = g_scene.lightColor.rgb * g_scene.lightColor.a;
    EvaluateBsdfLobes(surface, viewDirection, lightDirection, diffuseContribution, specularContribution);
    diffuseContribution *= radiance * visibility;
    specularContribution *= radiance * visibility;
    return diffuseContribution + specularContribution;
}

float3 EvaluateSkyNEE(
    SurfaceData surface,
    float3 viewDirection,
    float4 directionSample,
    out float3 diffuseContribution,
    out float3 specularContribution)
{
    diffuseContribution = 0.0f.xxx;
    specularContribution = 0.0f.xxx;
    if (g_scene.debugOptions.w < 0.5f)
    {
        return 0.0f.xxx;
    }

    float3 direction;
    float lightPdf;
    if (g_scene.environmentOptions.x > 0.5f)
    {
        SampleEnvironmentDirection(directionSample, direction, lightPdf);
    }
    else
    {
        direction = SampleCosineHemisphere(directionSample.xy, surface.normal);
        lightPdf = CosineHemispherePdf(surface.normal, direction);
    }
    if (lightPdf <= 0.0f || dot(surface.normal, direction) <= 0.0f)
    {
        return 0.0f.xxx;
    }
    float visibility = TraceVisibilityRay(
        surface.position,
        surface.normal,
        direction,
        g_scene.rayOptions.y,
        surface.rayConeWidth,
        surface.rayConeSpread);
    if (visibility <= 0.0f)
    {
        return 0.0f.xxx;
    }

    float3 sky = EvaluateSky(direction, surface.rayConeSpread);
    float bsdfPdf = EvaluateBsdfPdf(surface, viewDirection, direction);
    float misWeight = PowerHeuristic(lightPdf, bsdfPdf);
    EvaluateBsdfLobes(surface, viewDirection, direction, diffuseContribution, specularContribution);
    // AO is not part of the physical environment integrand and is not
    // applied by the competing BSDF-miss technique. Keeping both techniques
    // on the same integrand is required for unbiased MIS.
    float3 scale = sky * visibility * misWeight / lightPdf;
    diffuseContribution *= scale;
    specularContribution *= scale;
    return diffuseContribution + specularContribution;
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

float3 EvaluateAreaLightNEE(
    SurfaceData surface,
    float3 viewDirection,
    float lightSample,
    float2 uv,
    out float3 diffuseContribution,
    out float3 specularContribution)
{
    diffuseContribution = 0.0f.xxx;
    specularContribution = 0.0f.xxx;
    uint lightCount = (uint)round(g_scene.lightOptions.x);
    if (lightCount == 0u)
    {
        return 0.0f.xxx;
    }

    uint lightIndex = SelectLightIndex(lightSample, lightCount);
    RtLight light = g_lights[lightIndex];
    float3 edge0 = light.edge0Type.xyz;
    float3 edge1 = light.edge1.xyz;
    float type = light.edge0Type.w;
    float3 lightPosition;
    float3 lightBarycentrics = 0.0f.xxx;
    if (type < 0.5f)
    {
        // Uniform triangle sampling. Sampling the edge parallelogram while
        // dividing by triangle area previously doubled the estimator and
        // generated points outside the actual emitter.
        float rootU = sqrt(uv.x);
        float edgeWeight0 = rootU * (1.0f - uv.y);
        float edgeWeight1 = rootU * uv.y;
        lightBarycentrics = float3(1.0f - rootU, edgeWeight0, edgeWeight1);
        lightPosition = light.positionArea.xyz + edge0 * edgeWeight0 + edge1 * edgeWeight1;
    }
    else
    {
        lightPosition = light.positionArea.xyz + edge0 * uv.x + edge1 * uv.y;
    }
    float3 toLight = lightPosition - surface.position;
    float distanceSquared = max(dot(toLight, toLight), 0.0001f);
    float distanceToLight = sqrt(distanceSquared);
    float3 lightDirection = toLight / distanceToLight;
    float nDotL = saturate(dot(surface.normal, lightDirection));
    if (nDotL <= 0.0f)
    {
        return 0.0f.xxx;
    }

    float3 lightNormal = normalize(cross(edge0, edge1));
    float lightCos = abs(dot(lightNormal, -lightDirection));
    if (lightCos <= 0.0001f)
    {
        return 0.0f.xxx;
    }

    float selectionPdf = light.radianceCdf.w;
    if (selectionPdf <= 0.0f || light.positionArea.w <= 0.0f)
    {
        return 0.0f.xxx;
    }
    float areaPdf = selectionPdf / light.positionArea.w;
    float solidAnglePdf = areaPdf * distanceSquared / lightCos;
    float visibility = TraceVisibilityRay(
        surface.position,
        surface.normal,
        lightDirection,
        distanceToLight - g_scene.rayOptions.x * 2.0f,
        surface.rayConeWidth,
        surface.rayConeSpread);
    if (visibility <= 0.0f)
    {
        return 0.0f.xxx;
    }

    float3 radiance = type < 0.5f
        ? EvaluateMeshEmitterRadiance(
            light,
            lightBarycentrics,
            RayConeWidthAtDistance(surface.rayConeWidth, surface.rayConeSpread, distanceToLight),
            lightDirection)
        : light.radianceCdf.rgb * g_scene.lightOptions.z;
    float bsdfPdf = EvaluateBsdfPdf(surface, viewDirection, lightDirection);
    // Procedural rectangles are not present in the acceleration structure and
    // therefore have no competing BSDF-hit technique.
    float misWeight = type < 0.5f ? PowerHeuristic(solidAnglePdf, bsdfPdf) : 1.0f;
    EvaluateBsdfLobes(surface, viewDirection, lightDirection, diffuseContribution, specularContribution);
    float3 scale = radiance * visibility * misWeight / max(solidAnglePdf, 1.0e-10f);
    diffuseContribution *= scale;
    specularContribution *= scale;
    return diffuseContribution + specularContribution;
}

float MatchedEmissiveLightPdf(
    uint geometryIndex,
    uint primitiveIndex,
    float3 rayOrigin,
    float3 hitPosition,
    float3 rayDirection)
{
    // Mesh records are emitted in monotonically increasing
    // (geometry, primitive) order and analytic records use the all-ones
    // sentinel, so an integer lower-bound lookup replaces the old O(N)
    // floating-point geometry scan.
    uint lightCount = (uint)round(g_scene.lightOptions.x);
    uint lower = 0u;
    uint upper = lightCount;
    [loop]
    while (lower < upper)
    {
        uint middle = lower + (upper - lower) / 2u;
        uint2 identity = g_lights[middle].meshIdentity.xy;
        bool precedesTarget = identity.x < geometryIndex ||
            (identity.x == geometryIndex && identity.y < primitiveIndex);
        if (precedesTarget) lower = middle + 1u;
        else upper = middle;
    }

    if (lower >= lightCount)
    {
        return 0.0f;
    }
    RtLight light = g_lights[lower];
    if (light.edge0Type.w >= 0.5f ||
        light.meshIdentity.x != geometryIndex ||
        light.meshIdentity.y != primitiveIndex)
    {
        return 0.0f;
    }

    float selectionPdf = light.radianceCdf.w;
    if (selectionPdf <= 0.0f || light.positionArea.w <= 0.0f)
    {
        return 0.0f;
    }
    float3 lightNormal = normalize(cross(light.edge0Type.xyz, light.edge1.xyz));
    float lightCos = abs(dot(lightNormal, -rayDirection));
    float3 toLight = hitPosition - rayOrigin;
    float distanceSquared = max(dot(toLight, toLight), 1.0e-8f);
    return lightCos > 1.0e-6f
        ? (selectionPdf / light.positionArea.w) * distanceSquared / lightCos
        : 0.0f;
}

RayPayload EmptyPayload()
{
    RayPayload payload = (RayPayload)0;
    payload.barycentrics = 0.0f.xx;
    payload.hitT = 0.0f;
    payload.rayConeWidth = 0.0f;
    payload.rayConeSpread = 0.0f;
    payload.instanceIndex = 0xffffffffu;
    payload.geometryIndex = 0xffffffffu;
    payload.primitiveIndex = 0xffffffffu;
    return payload;
}

bool PayloadHit(RayPayload payload)
{
    return payload.geometryIndex != 0xffffffffu && payload.hitT > 0.0f;
}

SurfaceData EmptySurfaceData()
{
    SurfaceData surface = (SurfaceData)0;
    surface.normalTexture = 0.5f.xxx;
    surface.ao = 1.0f;
    surface.roughness = 1.0f;
    surface.materialIndex = 0xffffffffu;
    return surface;
}

PathHitData EmptyPathHitData()
{
    PathHitData hit = (PathHitData)0;
    hit.surface = EmptySurfaceData();
    hit.instanceIndex = 0xffffffffu;
    hit.geometryIndex = 0xffffffffu;
    hit.primitiveIndex = 0xffffffffu;
    return hit;
}

PathHitData MakePathHitData(RayPayload payload, SurfaceData surface)
{
    PathHitData hit = (PathHitData)0;
    hit.surface = surface;
    hit.hitT = payload.hitT;
    hit.hit = PayloadHit(payload) ? 1u : 0u;
    hit.instanceIndex = payload.instanceIndex;
    hit.geometryIndex = payload.geometryIndex;
    hit.primitiveIndex = payload.primitiveIndex;
    return hit;
}

SurfaceData LoadPayloadSurface(
    RayPayload payload,
    float3 rayOrigin,
    float3 rayDirection)
{
    float uvFootprint = TriangleUvFootprint(
        payload.geometryIndex,
        payload.primitiveIndex,
        payload.rayConeWidth,
        rayDirection);
    SurfaceData surface = LoadSurface(
        payload.geometryIndex,
        payload.primitiveIndex,
        payload.barycentrics,
        uvFootprint,
        payload.rayConeWidth,
        payload.rayConeSpread);
    // RayTCurrent is measured in world-ray parameter space. Reconstructing the
    // position here avoids carrying it and keeps closest-hit material-free.
    surface.position = rayOrigin + rayDirection * payload.hitT;
    return surface;
}

static const uint SampleDimensionCamera = 0u;
static const uint SampleDimensionBounceBase = 1u;
static const uint SampleDimensionsPerBounce = 11u;
static const uint SampleDimensionSkyDirection = 0u;
static const uint SampleDimensionSkyAlias = 2u;
static const uint SampleDimensionLightSelector = 4u;
static const uint SampleDimensionLightSurface = 5u;
static const uint SampleDimensionBsdfLobe = 7u;
static const uint SampleDimensionBsdfDirection = 8u;
static const uint SampleDimensionRussianRoulette = 10u;

uint BounceSampleDimension(uint bounce, uint dimensionOffset)
{
    return SampleDimensionBounceBase + bounce * SampleDimensionsPerBounce + dimensionOffset;
}

float3 GenerateCameraDirection(uint2 pixel, float2 sampleJitter)
{
    uint2 dimensions = DispatchRaysDimensions().xy;
    float2 uv = (float2(pixel) + 0.5f.xx + sampleJitter) / float2(dimensions);
    float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    float4 nearPoint = mul(float4(ndc, 0.0f, 1.0f), g_scene.inverseViewProjection);
    float4 farPoint = mul(float4(ndc, 1.0f, 1.0f), g_scene.inverseViewProjection);
    nearPoint.xyz /= nearPoint.w;
    farPoint.xyz /= farPoint.w;
    return normalize(farPoint.xyz - nearPoint.xyz);
}

float3 GenerateCameraDirection(uint2 pixel)
{
    return GenerateCameraDirection(pixel, g_scene.jitterOptions.xy);
}

float CameraPixelConeSpread(uint2 pixel, float2 sampleJitter)
{
    return max(g_scene.performanceOptions.x, 1.0e-6f);
}

bool GatherAdaptiveReprojectedLuminance(
    uint2 pixel,
    uint2 dimensions,
    PathHitData currentHitData,
    out float historyLuminance);

bool ShouldTraceAdaptiveSecondary(
    uint2 pixel,
    RayPayload primaryPayload,
    SurfaceData surface,
    float3 viewDirection)
{
    if (g_scene.performanceOptions.y >= 0.75f)
    {
        return true;
    }

    // Alternate the traced half every submitted frame. Primary visibility and
    // all primary-vertex NEE have already run before this test; only the BSDF
    // continuation ray and later vertices are eligible for half-rate shading.
    uint checkerboardParity = (pixel.x + pixel.y + g_scene.frameOptions.w) & 1u;
    if (checkerboardParity == 0u)
    {
        return true;
    }

    uint historyDomains = (uint)round(g_scene.environmentOptions.w);
    const uint commonHistory = HISTORY_DOMAIN_SURFACE | HISTORY_DOMAIN_LIGHTING;
    bool internalHistoryValid = g_scene.denoiseOptions.x > 0.5f &&
        (historyDomains & (commonHistory | HISTORY_DOMAIN_DENOISER)) ==
            (commonHistory | HISTORY_DOMAIN_DENOISER);
    bool taaHistoryValid = (historyDomains & (commonHistory | HISTORY_DOMAIN_TAA)) ==
        (commonHistory | HISTORY_DOMAIN_TAA);
    if (!internalHistoryValid && !taaHistoryValid)
    {
        // There is no temporal reconstruction source for a skipped lobe.
        return true;
    }

    // Preserve visibility-sensitive and high-frequency surfaces at full rate.
    // Alpha-masked geometry is kept full even when this particular texel is
    // opaque so a moving coverage edge cannot alternate between estimators.
    RtMaterial primaryMaterial = surface.material;
    if (primaryMaterial.alphaMasked != 0u || surface.coverage < 0.999f ||
        surface.roughness < 0.15f || surface.metallic > 0.5f ||
        BsdfSpecularProbability(surface, viewDirection) > 0.75f)
    {
        return true;
    }

    if (internalHistoryValid)
    {
        float4 historyControls = LoadPreviousHistoryLength(pixel);
        float4 historyMoments = LoadPreviousHistoryMoments(pixel);
        float diffuseVariance = max(historyMoments.y - historyMoments.x * historyMoments.x, 0.0f);
        float specularVariance = max(historyMoments.w - historyMoments.z * historyMoments.z, 0.0f);
        float historyLength = min(historyControls.x, historyControls.y);
        float reactive = max(historyControls.z, historyControls.w);
        if (historyLength < 2.0f || reactive > 0.25f ||
            max(diffuseVariance, specularVariance) > g_scene.adaptiveOptions.z)
        {
            return true;
        }
    }

    if (taaHistoryValid)
    {
        float reprojectedLuminance;
        if (!GatherAdaptiveReprojectedLuminance(
                pixel,
                DispatchRaysDimensions().xy,
                MakePathHitData(primaryPayload, surface),
                reprojectedLuminance))
        {
            // Reprojection rejection is a disocclusion, so never synthesize its
            // first indirect sample from unrelated temporal neighbors.
            return true;
        }

        // Fusion does not publish the redundant post-denoise intermediate. Its
        // immutable previous resolve is already available in the TAA ping-pong;
        // retain the legacy source for every non-fused backend.
        float3 previousFiltered = g_scene.postProcessOptions.x > 0.5f
            ? (PreviousHistoryIsA() ? g_taaHistoryA[pixel].rgb : g_taaHistoryB[pixel].rgb)
            : g_postDenoiseHdr[pixel].rgb;
        float previousFilteredLuminance = Luminance(max(previousFiltered, 0.0f.xxx));
        float temporalResidual = abs(previousFilteredLuminance - reprojectedLuminance) /
            max(max(previousFilteredLuminance, reprojectedLuminance), 0.25f);
        float previousSecondaryConfidence = max(
            g_diffuseHistoryConfidence[pixel],
            g_specularHistoryConfidence[pixel]);
        if (previousSecondaryConfidence < 0.25f ||
            temporalResidual > max(g_scene.signalDenoiseOptions.z, g_scene.adaptiveOptions.z))
        {
            return true;
        }
    }

    return false;
}

float3 TracePathSample(
    uint2 pixel,
    uint sampleIndex,
    float2 cameraJitter,
    out PathHitData firstHit,
    out float3 diffuseSignal,
    out float3 specularSignal,
    out uint bounceCount,
    out float diffuseHitDistance,
    out float specularHitDistance,
    out float inputContributionEnergy,
    out float outputContributionEnergy)
{
    float3 rayOrigin = g_scene.cameraPosition.xyz;
    float3 rayDirection = GenerateCameraDirection(pixel, cameraJitter);
    float rayConeWidth = 0.0f;
    float rayConeSpread = CameraPixelConeSpread(pixel, cameraJitter);
    float3 throughput = 1.0f.xxx;
    float3 radiance = 0.0f.xxx;
    diffuseSignal = 0.0f.xxx;
    specularSignal = 0.0f.xxx;
    bounceCount = 0u;
    diffuseHitDistance = 0.0f;
    specularHitDistance = 0.0f;
    inputContributionEnergy = 0.0f;
    outputContributionEnergy = 0.0f;
    firstHit = EmptyPathHitData();
    bool firstContinuationWasSpecular = false;
    float previousBsdfPdf = 0.0f;
    float3 previousSurfaceNormal = 0.0f.xxx;

    uint maxBounces = clamp((uint)round(g_scene.pathOptions.x), 1u, 8u);
    uint minBounces = min((uint)round(g_scene.pathOptions.y), maxBounces);

    for (uint bounce = 0u; bounce < 8u; ++bounce)
    {
        if (bounce >= maxBounces)
        {
            break;
        }

        RayPayload payload = EmptyPayload();
        payload.rayConeWidth = rayConeWidth;
        payload.rayConeSpread = rayConeSpread;
        RayDesc ray;
        ray.Origin = rayOrigin;
        ray.Direction = rayDirection;
        ray.TMin = g_scene.rayOptions.x;
        ray.TMax = g_scene.rayOptions.y;
        TraceRay(g_sceneAs, RAY_FLAG_NONE, 0xff, 0, 0, 0, ray, payload);

        if (bounce == 1u)
        {
            float secondaryDistance = PayloadHit(payload) ? payload.hitT : g_scene.rayOptions.y;
            if (firstContinuationWasSpecular)
            {
                specularHitDistance = secondaryDistance;
            }
            else
            {
                diffuseHitDistance = secondaryDistance;
            }
        }

        if (!PayloadHit(payload))
        {
            float3 sky = EvaluateSky(rayDirection, rayConeSpread);
            float skyMisWeight = 1.0f;
            if (bounce > 0u && g_scene.debugOptions.w > 0.5f)
            {
                float skyLightPdf = SkyNeePdf(previousSurfaceNormal, rayDirection);
                skyMisWeight = PowerHeuristic(previousBsdfPdf, skyLightPdf);
            }
            float3 skyContribution = throughput * sky * skyMisWeight;
            radiance += skyContribution;
            if (bounce > 0u)
            {
                if (firstContinuationWasSpecular)
                {
                    specularSignal += skyContribution;
                }
                else
                {
                    diffuseSignal += skyContribution;
                }
            }
            break;
        }

        SurfaceData surface = LoadPayloadSurface(payload, rayOrigin, rayDirection);
        surface.normal = dot(surface.normal, -rayDirection) > 0.0f ? surface.normal : -surface.normal;
        if (bounce == 0u)
        {
            firstHit = MakePathHitData(payload, surface);
        }

        float emissiveLightPdf = MaxComponent(surface.emissive) > 0.0f
            ? MatchedEmissiveLightPdf(
                payload.geometryIndex,
                payload.primitiveIndex,
                rayOrigin,
                surface.position,
                rayDirection)
            : 0.0f;
        float emissiveMisWeight = 1.0f;
        if (bounce > 0u && emissiveLightPdf > 0.0f)
        {
            emissiveMisWeight = PowerHeuristic(previousBsdfPdf, emissiveLightPdf);
        }
        float3 emissiveContribution = throughput * surface.emissive * emissiveMisWeight;
        radiance += emissiveContribution;
        if (bounce > 0u)
        {
            if (firstContinuationWasSpecular)
            {
                specularSignal += emissiveContribution;
            }
            else
            {
                diffuseSignal += emissiveContribution;
            }
        }
        float3 viewDirection = normalize(-rayDirection);
        float3 sunDiffuse;
        float3 sunSpecular;
        float3 skyDiffuse = 0.0f.xxx;
        float3 skySpecular = 0.0f.xxx;
        float3 areaDiffuse = 0.0f.xxx;
        float3 areaSpecular = 0.0f.xxx;
        EvaluateSunNEE(surface, viewDirection, sunDiffuse, sunSpecular);
        if (g_scene.debugOptions.w > 0.5f)
        {
            float4 skyDirectionSample = float4(
                OwenScrambledSobol2D(pixel, sampleIndex, BounceSampleDimension(bounce, SampleDimensionSkyDirection)),
                OwenScrambledSobol2D(pixel, sampleIndex, BounceSampleDimension(bounce, SampleDimensionSkyAlias)));
            EvaluateSkyNEE(surface, viewDirection, skyDirectionSample, skyDiffuse, skySpecular);
        }
#if PT_RESTIR_DI
        // RTXDI owns primary local-light direct illumination. Keep Baseline
        // NEE for secondary vertices so indirect transport remains available
        // while ReSTIR GI/PT is still an explicit fallback.
        if (bounce > 0u && g_scene.lightOptions.x >= 0.5f)
        {
            float lightSelectorSample = OwenScrambledSobol1D(pixel, sampleIndex, BounceSampleDimension(bounce, SampleDimensionLightSelector));
            float2 lightSurfaceSample = OwenScrambledSobol2D(pixel, sampleIndex, BounceSampleDimension(bounce, SampleDimensionLightSurface));
            EvaluateAreaLightNEE(surface, viewDirection, lightSelectorSample, lightSurfaceSample, areaDiffuse, areaSpecular);
        }
#else
        if (g_scene.lightOptions.x >= 0.5f)
        {
            float lightSelectorSample = OwenScrambledSobol1D(pixel, sampleIndex, BounceSampleDimension(bounce, SampleDimensionLightSelector));
            float2 lightSurfaceSample = OwenScrambledSobol2D(pixel, sampleIndex, BounceSampleDimension(bounce, SampleDimensionLightSurface));
            EvaluateAreaLightNEE(surface, viewDirection, lightSelectorSample, lightSurfaceSample, areaDiffuse, areaSpecular);
        }
#endif
        float3 diffuseNee = sunDiffuse + skyDiffuse + areaDiffuse;
        float3 specularNee = sunSpecular + skySpecular + areaSpecular;
        // Light sampling already evaluates both BSDF lobes at the sampled
        // direction. Summing them directly removes an unnecessary stochastic
        // lobe selector from NEE and keeps the denoiser signals demodulated.
        float3 diffuseNeeContribution = throughput * diffuseNee;
        float3 specularNeeContribution = throughput * specularNee;
        float3 neeContribution = diffuseNeeContribution + specularNeeContribution;
        radiance += neeContribution;
        if (bounce == 0u)
        {
            diffuseSignal += diffuseNeeContribution;
            specularSignal += specularNeeContribution;
        }
        else if (firstContinuationWasSpecular)
        {
            specularSignal += neeContribution;
        }
        else
        {
            diffuseSignal += neeContribution;
        }

        // NEE and emissive evaluation are the only contributions at the final
        // vertex. Do not generate a direction, evaluate PDFs/BSDF throughput,
        // or run roulette for a ray that the loop can never trace.
        bounceCount = bounce + 1u;
        if (bounce + 1u >= maxBounces)
        {
            break;
        }
        if (bounce == 0u && !ShouldTraceAdaptiveSecondary(pixel, payload, surface, viewDirection))
        {
            // diffuse/specular hit distances stay at their zero initialization.
            // NRD interprets that sentinel as a missing checkerboard sample and
            // reconstructs it, while primary direct lighting remains present.
            break;
        }

        float specularProbability = BsdfSpecularProbability(surface, viewDirection);
        float chooseSpecular = OwenScrambledSobol1D(pixel, sampleIndex, BounceSampleDimension(bounce, SampleDimensionBsdfLobe));
        float2 bsdfDirectionSample = OwenScrambledSobol2D(pixel, sampleIndex, BounceSampleDimension(bounce, SampleDimensionBsdfDirection));
        float3 nextDirection;
        float scatterConeSpread;
        bool sampledSpecular = chooseSpecular < specularProbability;

        if (sampledSpecular)
        {
            if (bounce == 0u)
            {
                firstContinuationWasSpecular = true;
            }
            nextDirection = SampleGGXDirection(bsdfDirectionSample, surface.roughness, surface.normal, viewDirection);
            scatterConeSpread = surface.roughness * surface.roughness * 0.25f;
        }
        else
        {
            if (bounce == 0u)
            {
                firstContinuationWasSpecular = false;
            }
            nextDirection = SampleCosineHemisphere(bsdfDirectionSample, surface.normal);
            scatterConeSpread = 0.25f;
        }

        float conditionalPdf = sampledSpecular
            ? GGXReflectionPdf(surface, viewDirection, nextDirection)
            : CosineHemispherePdf(surface.normal, nextDirection);
        float mixturePdf = EvaluateBsdfPdf(surface, viewDirection, nextDirection);
        if (conditionalPdf <= 1.0e-10f || mixturePdf <= 1.0e-10f || dot(surface.normal, nextDirection) <= 0.0f)
        {
            break;
        }
        float3 diffuseBsdf;
        float3 specularBsdf;
        EvaluateBsdfLobes(surface, viewDirection, nextDirection, diffuseBsdf, specularBsdf);
        // The direction comes from the diffuse/specular mixture, so evaluate
        // the full BSDF and divide by that same mixture PDF. Dividing only the
        // selected lobe by its conditional PDF biases overlapping lobes and
        // makes the PDF used by emissive/environment MIS inconsistent.
        float3 bsdfWeight = (diffuseBsdf + specularBsdf) / max(mixturePdf, 1.0e-10f);
        throughput *= bsdfWeight;
        if (g_scene.giOptions.y > 0.0f)
        {
            throughput = min(throughput, 16.0f.xxx);
        }
        previousBsdfPdf = mixturePdf;
        previousSurfaceNormal = surface.normal;
        rayOrigin = surface.position + surface.normal * g_scene.rayOptions.x;
        rayDirection = normalize(nextDirection);
        rayConeWidth = payload.rayConeWidth;
        rayConeSpread = min(max(payload.rayConeSpread, scatterConeSpread), 0.5f);
        if (bounce + 1u >= minBounces)
        {
            float continueProbability = clamp(MaxComponent(throughput), 0.05f, 0.95f);
            float rouletteSample = OwenScrambledSobol1D(pixel, sampleIndex, BounceSampleDimension(bounce, SampleDimensionRussianRoulette));
            if (rouletteSample > continueProbability)
            {
                break;
            }
            throughput /= continueProbability;
        }
    }

    // Interactive contribution compression must preserve the signal split.
    // Clamping only the combined estimator while leaving the demodulated
    // diffuse/specular outputs untouched feeds arbitrarily large fireflies to
    // NRD, and recombining those lobes no longer matches the beauty estimator.
    // Apply one common scale after the path is complete so
    // diffuse + specular + residual remains the same compressed estimate.
    float3 compressedRadiance = radiance;
    float contributionLimit = max(g_scene.giOptions.y, 0.0f);
    float contributionLuminance = Luminance(compressedRadiance);
    inputContributionEnergy = contributionLuminance;
    if (contributionLimit > 0.0f && contributionLuminance > contributionLimit)
    {
        float compression = contributionLimit / max(contributionLuminance, 1.0e-6f);
        compressedRadiance *= compression;
        diffuseSignal *= compression;
        specularSignal *= compression;
    }
    outputContributionEnergy = Luminance(compressedRadiance);
    return compressedRadiance;
}

float2 ProjectWorldToUv(float3 worldPosition, float4x4 viewProjection)
{
    float4 clip = mul(float4(worldPosition, 1.0f), viewProjection);
    float2 ndc = clip.xy / max(abs(clip.w), 0.0001f);
    return float2(ndc.x * 0.5f + 0.5f, 0.5f - ndc.y * 0.5f);
}

float ProjectWorldToViewZ(float3 worldPosition, float4x4 viewProjection)
{
    // For the D3D perspective matrix used by the host, clip.w is linear
    // camera-space Z. Keep it positive because the denoisers use positive view-Z.
    float4 clip = mul(float4(worldPosition, 1.0f), viewProjection);
    return max(abs(clip.w), 0.0001f);
}

float4 LoadPreviousTaaHistory(uint2 pixel)
{
    return PreviousHistoryIsA() ? g_taaHistoryA[pixel] : g_taaHistoryB[pixel];
}

bool GatherAdaptiveReprojectedLuminance(
    uint2 pixel,
    uint2 dimensions,
    PathHitData currentHitData,
    out float historyLuminance)
{
    historyLuminance = 0.0f;
    uint historyDomains = (uint)round(g_scene.environmentOptions.w);
    if ((historyDomains & (HISTORY_DOMAIN_SURFACE | HISTORY_DOMAIN_TAA)) !=
        (HISTORY_DOMAIN_SURFACE | HISTORY_DOMAIN_TAA))
    {
        return false;
    }

    bool currentHit = currentHitData.hit != 0u;
    SurfaceData currentSurface = currentHitData.surface;
    float2 previousUv;
    float expectedPreviousViewZ = 0.0f;
    uint currentIdentity = 0u;
    if (currentHit)
    {
        previousUv = ProjectWorldToUv(currentSurface.position, g_scene.previousViewProjection);
        expectedPreviousViewZ = ProjectWorldToViewZ(currentSurface.position, g_scene.previousViewProjection);
        currentIdentity = PackSurfaceIdentity(
            currentHitData.instanceIndex,
            currentHitData.geometryIndex,
            currentHitData.primitiveIndex,
            currentSurface.materialIndex,
            currentSurface.coverage);
    }
    else
    {
        float3 direction = GenerateCameraDirection(pixel);
        float3 distantPoint = g_scene.cameraPosition.xyz + direction * 100000.0f;
        previousUv = ProjectWorldToUv(distantPoint, g_scene.previousViewProjection);
    }

    float2 jitterDelta = g_scene.jitterOptions.xy - g_scene.jitterOptions.zw;
    // previousUv is an absolute non-jittered projection of the current hit.
    // Convert it to the resolved output grid by removing the current jitter.
    // Adding current-previous jitter here (the old code) counted current jitter
    // twice and made the adaptive classifier chase the wrong history footprint.
    float2 historyPosition = previousUv * float2(dimensions) - 0.5f.xx - g_scene.jitterOptions.xy;
    int2 basePixel = int2(floor(historyPosition));
    float2 fraction = frac(historyPosition);
    float weightedLuminance = 0.0f;
    float totalWeight = 0.0f;
    [unroll]
    for (int y = 0; y < 2; ++y)
    {
        [unroll]
        for (int x = 0; x < 2; ++x)
        {
            int2 candidate = basePixel + int2(x, y);
            if (any(candidate < 0) || any(candidate >= int2(dimensions)))
            {
                continue;
            }
            int2 guideCandidate = int2(round(float2(candidate) + jitterDelta));
            if (any(guideCandidate < 0) || any(guideCandidate >= int2(dimensions)))
            {
                continue;
            }
            uint2 previousPixel = uint2(guideCandidate);
            float4 previousAov2 = g_previousDenoiseAov2[previousPixel];
            bool previousHit = previousAov2.w > 0.0f;
            if (currentHit != previousHit)
            {
                continue;
            }
            if (currentHit)
            {
                float4 previousAov0 = g_previousDenoiseAov0[previousPixel];
                float4 previousAov1 = g_previousDenoiseAov1[previousPixel];
                float3 previousNormal = normalize(previousAov0.xyz * 2.0f - 1.0f);
                float relativeDepth = abs(previousAov0.w - expectedPreviousViewZ) /
                    max(abs(expectedPreviousViewZ), 1.0f);
                if (!ValidatePackedSurfaceIdentity(currentIdentity, g_previousSurfaceIdentity[previousPixel]) ||
                    dot(currentSurface.normal, previousNormal) < g_scene.validationOptions.x ||
                    relativeDepth > g_scene.validationOptions.y ||
                    length(currentSurface.baseColor - previousAov1.rgb) > g_scene.validationOptions.z ||
                    abs(currentSurface.roughness - previousAov1.w) > g_scene.validationOptions.w)
                {
                    continue;
                }
            }

            float weight = (x == 0 ? 1.0f - fraction.x : fraction.x) *
                (y == 0 ? 1.0f - fraction.y : fraction.y);
            float4 history = LoadPreviousTaaHistory(uint2(candidate));
            if (history.a <= 0.0f)
            {
                continue;
            }
            weightedLuminance += Luminance(max(history.rgb, 0.0f.xxx)) * weight;
            totalWeight += weight;
        }
    }
    if (totalWeight <= 1.0e-6f)
    {
        return false;
    }
    historyLuminance = weightedLuminance / totalWeight;
    return true;
}

[shader("raygeneration")]
void RayGen()
{
    uint2 pixel = DispatchRaysIndex().xy;
    uint2 dimensions = DispatchRaysDimensions().xy;
    uint accumulatedFrames = g_scene.frameOptions.x;
    uint maxAccumulatedFrames = max(g_scene.frameOptions.y, 1u);
    uint frameCounter = g_scene.frameOptions.w;
    uint samplesPerFrame = clamp((uint)round(g_scene.giOptions.x), 1u, 8u);
    // ReSTIR DI candidates are generated after this primary/full-path pass.
    // The DI library variant above omits primary local-light NEE and the
    // compute chain adds the current-surface RTXDI estimator before denoising.
    uint totalSamples = samplesPerFrame;
    bool adaptiveEnabled = g_scene.adaptiveOptions.x > 0.5f;
    uint adaptiveMax = clamp((uint)round(g_scene.adaptiveOptions.y), 1u, 4u);
    // The internal fallback owns explicit luminance moments. Its first-stage
    // classification can run before tracing; external backends use the first
    // primary/path sample plus immutable TAA history below.
    if (adaptiveEnabled && g_scene.denoiseOptions.x > 0.5f)
    {
        uint historyDomains = (uint)round(g_scene.environmentOptions.w);
        bool historyValid = (historyDomains & (HISTORY_DOMAIN_SURFACE | HISTORY_DOMAIN_DENOISER)) ==
            (HISTORY_DOMAIN_SURFACE | HISTORY_DOMAIN_DENOISER);
        float4 historyInfo = historyValid ? LoadPreviousHistoryLength(pixel) : 0.0f.xxxx;
        float4 historyMoments = historyValid ? LoadPreviousHistoryMoments(pixel) : 0.0f.xxxx;
        // Fallback moments are packed as diffuse(mean, mean2) and
        // specular(mean, mean2). Sample where either estimator is unstable.
        float diffuseVariance = max(historyMoments.y - historyMoments.x * historyMoments.x, 0.0f);
        float specularVariance = max(historyMoments.w - historyMoments.z * historyMoments.z, 0.0f);
        float variance = max(diffuseVariance, specularVariance);
        float historyLength = min(historyInfo.x, historyInfo.y);
        float disocclusion = historyValid && historyLength > 0.0f ? max(historyInfo.z, historyInfo.w) : 1.0f;
        if (variance > g_scene.adaptiveOptions.z || historyLength < 2.0f || disocclusion > 0.5f)
        {
            totalSamples = max(totalSamples, adaptiveMax);
        }
    }
    // Reserve a fixed 32-sample block per frame. Adaptive sample-count changes
    // can then neither repeat nor rewind a pixel's temporal sample sequence.
    uint firstSampleIndex = frameCounter * 32u;

    PathHitData firstHit = EmptyPathHitData();
    float3 color = 0.0f.xxx;
    float3 diffuseSignal = 0.0f.xxx;
    float3 specularSignal = 0.0f.xxx;
    uint bounceCount = 0u;
    float diffuseHitDistanceSum = 0.0f;
    float specularHitDistanceSum = 0.0f;
    uint diffuseHitDistanceCount = 0u;
    uint specularHitDistanceCount = 0u;
    float inputContributionEnergySum = 0.0f;
    float outputContributionEnergySum = 0.0f;

    for (uint i = 0u; i < 32u; ++i)
    {
        if (i >= totalSamples)
        {
            break;
        }
        PathHitData sampleFirstHit;
        float3 sampleDiffuse;
        float3 sampleSpecular;
        uint sampleBounces;
        float sampleDiffuseHitDistance;
        float sampleSpecularHitDistance;
        float sampleInputContributionEnergy;
        float sampleOutputContributionEnergy;
        uint pathSampleIndex = firstSampleIndex + i;
        // Keep sample zero aligned with the host-provided temporal jitter: its
        // first-hit data is the guide surface consumed by the denoisers. Extra
        // SPP use distinct Sobol camera samples, with that jitter as a global
        // Cranley-Patterson rotation inside the pixel.
        float2 cameraJitter = g_scene.jitterOptions.xy;
        if (i > 0u)
        {
            float2 cameraSample = OwenScrambledSobol2D(pixel, pathSampleIndex, SampleDimensionCamera);
            cameraJitter = frac(cameraSample + g_scene.jitterOptions.xy) - 0.5f.xx;
        }
        float3 sampleColor = TracePathSample(
            pixel,
            pathSampleIndex,
            cameraJitter,
            sampleFirstHit,
            sampleDiffuse,
            sampleSpecular,
            sampleBounces,
            sampleDiffuseHitDistance,
            sampleSpecularHitDistance,
            sampleInputContributionEnergy,
            sampleOutputContributionEnergy);
        color += sampleColor;
        inputContributionEnergySum += sampleInputContributionEnergy;
        outputContributionEnergySum += sampleOutputContributionEnergy;
        if (i == 0u)
        {
            firstHit = sampleFirstHit;
            if (adaptiveEnabled && g_scene.denoiseOptions.x <= 0.5f && totalSamples < adaptiveMax)
            {
                // Stage two runs only after primary visibility is known. A
                // validated reprojected HDR residual is the external-backend
                // variance proxy; a rejected surface is a disocclusion.
                float historyLuminance;
                bool validHistory = GatherAdaptiveReprojectedLuminance(
                    pixel,
                    dimensions,
                    sampleFirstHit,
                    historyLuminance);
                float sampleLuminance = Luminance(max(sampleColor, 0.0f.xxx));
                float temporalResidual = validHistory
                    ? abs(sampleLuminance - historyLuminance) / max(max(sampleLuminance, historyLuminance), 0.25f)
                    : 1.0f;
                if (!validHistory || temporalResidual > g_scene.adaptiveOptions.z)
                {
                    totalSamples = adaptiveMax;
                }
            }
        }
        diffuseSignal += sampleDiffuse;
        specularSignal += sampleSpecular;
        bounceCount += sampleBounces;
        if (sampleDiffuseHitDistance > 0.0f)
        {
            diffuseHitDistanceSum += sampleDiffuseHitDistance;
            diffuseHitDistanceCount++;
        }
        if (sampleSpecularHitDistance > 0.0f)
        {
            specularHitDistanceSum += sampleSpecularHitDistance;
            specularHitDistanceCount++;
        }
    }

    color /= (float)totalSamples;
    diffuseSignal /= (float)totalSamples;
    specularSignal /= (float)totalSamples;
    float inputContributionEnergy = inputContributionEnergySum / (float)totalSamples;
    float outputContributionEnergy = outputContributionEnergySum / (float)totalSamples;
    float averageBounces = (float)bounceCount / (float)max(totalSamples, 1u);
    float diffuseHitDistance = diffuseHitDistanceSum / (float)max(diffuseHitDistanceCount, 1u);
    float specularHitDistance = specularHitDistanceSum / (float)max(specularHitDistanceCount, 1u);
    uint debugMode = (uint)round(g_scene.debugOptions.x);
    float3 debugColor = color;
    if (debugMode == 1u) debugColor = firstHit.surface.baseColor;
    if (debugMode == 2u) debugColor = firstHit.hit != 0u ? firstHit.surface.normal * 0.5f + 0.5f : 0.0f.xxx;
    if (debugMode == 3u) debugColor = firstHit.surface.normalTexture;
    if (debugMode == 4u) debugColor = firstHit.surface.roughness.xxx;
    if (debugMode == 5u) debugColor = firstHit.surface.metallic.xxx;
    if (debugMode == 6u) debugColor = firstHit.surface.emissive;
    if (debugMode == 7u) debugColor = firstHit.hit != 0u ? saturate(firstHit.hitT / 250.0f).xxx : 0.0f.xxx;
    if (debugMode == 8u) debugColor = diffuseSignal;
    if (debugMode == 9u) debugColor = specularSignal;
    if (debugMode == 10u) debugColor = (averageBounces / max(g_scene.pathOptions.x, 1.0f)).xxx;
    if (debugMode == 12u)
    {
        float3 skyDirection = GenerateCameraDirection(pixel);
        debugColor = EvaluateSky(skyDirection, CameraPixelConeSpread(pixel, g_scene.jitterOptions.xy));
    }
    color = ApplyMaterialFocus(color, firstHit.surface.materialIndex, firstHit.hit);
    diffuseSignal = ApplyMaterialFocus(diffuseSignal, firstHit.surface.materialIndex, firstHit.hit);
    specularSignal = ApplyMaterialFocus(specularSignal, firstHit.surface.materialIndex, firstHit.hit);
    debugColor = ApplyMaterialFocus(debugColor, firstHit.surface.materialIndex, firstHit.hit);
    float3 currentFrameColor = color;
    float3 residualSignal = max(currentFrameColor - diffuseSignal - specularSignal, 0.0f.xxx);
    const bool writeRealtimeSignals = g_scene.viewOptions.w > 0.5f;
    if (writeRealtimeSignals)
    {
        // The resource names predate signal separation. They now carry primary
        // diffuse and specular estimators respectively; alpha is the matching
        // first secondary in-lobe hit distance (0 for the skipped lobe).
        g_signalDirect[pixel] = float4(max(diffuseSignal, 0.0f.xxx), diffuseHitDistance);
        g_signalIndirect[pixel] = float4(max(specularSignal, 0.0f.xxx), specularHitDistance);
        // Residual alpha carries metallic so the redundant combined-radiance
        // surface can remain a 1x1 ABI placeholder.
        g_signalResidual[pixel] = float4(residualSignal, firstHit.surface.metallic);
    }

    if (debugMode == 0u)
    {
#if PT_RESTIR_DI
        // Progressive RGB accumulation and reservoir temporal reuse are
        // distinct estimators. Keep the RTXDI realtime path frame-local here;
        // its own immutable reservoir history plus the denoiser/TAA provide
        // temporal stability without recursively accumulating direct light.
        debugColor = color;
        g_accumulation[pixel] = float4(color, 1.0f);
#else
        if (g_scene.frameOptions.z != 0u)
        {
            debugColor = g_accumulation[pixel].rgb;
        }
        else
        {
            float3 history = g_accumulation[pixel].rgb;
            debugColor = ApplyTemporalClamp(debugColor, history, accumulatedFrames);
            float weight = 1.0f / (float)(min(accumulatedFrames, maxAccumulatedFrames - 1u) + 1u);
            debugColor = lerp(history, debugColor, weight);
            g_accumulation[pixel] = float4(debugColor, 1.0f);
        }
#endif
    }

    if (debugMode == 11u)
    {
        debugColor = ((float)accumulatedFrames / (float)maxAccumulatedFrames).xxx;
    }

    float firstHitDepth = -1.0f;
    float3 firstHitNormal = firstHit.hit != 0u ? firstHit.surface.normal * 0.5f + 0.5f : 0.0f.xxx;
    float2 motionVector = 0.0f.xx;
    float motionViewZ = 0.0f;
    if (firstHit.hit != 0u)
    {
        float2 currentUv = ProjectWorldToUv(firstHit.surface.position, g_scene.viewProjection);
        float2 previousUv = ProjectWorldToUv(firstHit.surface.position, g_scene.previousViewProjection);
        float currentViewZ = ProjectWorldToViewZ(firstHit.surface.position, g_scene.viewProjection);
        float previousViewZ = ProjectWorldToViewZ(firstHit.surface.position, g_scene.previousViewProjection);
        motionVector = previousUv - currentUv;
        motionViewZ = previousViewZ - currentViewZ;
        firstHitDepth = currentViewZ;
    }
    if (debugMode > 15u)
    {
        g_accumulation[pixel] = float4(color, 1.0f);
    }
    if (writeRealtimeSignals)
    {
        float surfaceSampleConfidence = firstHit.hit != 0u ? 1.0f : 0.0f;
        float diffuseSampleConfidence = diffuseHitDistance > 0.0f ? 1.0f : 0.0f;
        float specularSampleConfidence = specularHitDistance > 0.0f ? 1.0f : 0.0f;
        specularSampleConfidence *= lerp(0.55f, 1.0f, saturate(firstHit.surface.roughness));
        // Initialize artifact-visible confidence even when denoising is OFF.
        // Active NRD or fallback temporal reconstruction replaces these sample
        // confidences with validated history confidence later in the frame.
        g_diffuseHistoryConfidence[pixel] = saturate(surfaceSampleConfidence * diffuseSampleConfidence);
        g_specularHistoryConfidence[pixel] = saturate(surfaceSampleConfidence * specularSampleConfidence);
        g_denoiseAov0[pixel] = float4(firstHitNormal, firstHitDepth);
        g_denoiseAov1[pixel] = float4(firstHit.surface.baseColor, firstHit.surface.roughness);
        // motionHit.w is the primary hit distance, not a boolean. Zero is the
        // unique miss sentinel and positive values preserve the ray footprint
        // information needed by ReSTIR/NRD without reconstructing it again.
        g_denoiseAov2[pixel] = float4(
            motionVector,
            motionViewZ,
            firstHit.hit != 0u ? max(firstHit.hitT, 1.0e-6f) : 0.0f);
        g_surfaceIdentity[pixel] = firstHit.hit != 0u
            ? PackSurfaceIdentity(
                firstHit.instanceIndex,
                firstHit.geometryIndex,
                firstHit.primitiveIndex,
                firstHit.surface.materialIndex,
                firstHit.surface.coverage)
            : 0u;
        // This is also the input for final TAA when every denoiser backend is
        // off. Fused NRD + TAA reconstructs directly from the split signals and
        // NRD outputs, so publishing this full-resolution fallback is redundant.
        if (g_scene.postProcessOptions.x < 0.5f)
        {
            g_postDenoiseHdr[pixel] = float4(max(debugColor, 0.0f.xxx), 1.0f);
        }
    }

    if (QualityContributionEnabled(dimensions))
    {
        g_qualityContribution[pixel] = PackQualityContributionEnergy(
            inputContributionEnergy,
            outputContributionEnergy);
    }

    g_output[pixel] = float4(Tonemap(debugColor), 1.0f);
}

[shader("miss")]
void Miss(inout RayPayload payload)
{
    payload = EmptyPayload();
}

[shader("miss")]
void ShadowMiss(inout ShadowPayload payload)
{
    payload.occluded = 0;
}

[shader("anyhit")]
void AnyHit(inout RayPayload payload, in BuiltInTriangleIntersectionAttributes attributes)
{
    if (IsAlphaTransparent(
        GeometryIndex(),
        PrimitiveIndex(),
        attributes.barycentrics,
        payload.rayConeWidth,
        payload.rayConeSpread,
        RayTCurrent(),
        WorldRayDirection()))
    {
        IgnoreHit();
    }
}

[shader("anyhit")]
void ShadowAnyHit(inout ShadowPayload payload, in BuiltInTriangleIntersectionAttributes attributes)
{
    if (IsAlphaTransparent(
        GeometryIndex(),
        PrimitiveIndex(),
        attributes.barycentrics,
        payload.rayConeWidth,
        payload.rayConeSpread,
        RayTCurrent(),
        WorldRayDirection()))
    {
        IgnoreHit();
    }
}

[shader("closesthit")]
void ClosestHit(inout RayPayload payload, in BuiltInTriangleIntersectionAttributes attributes)
{
    // Intersection-only closest hit: all vertex/material/texture work is done
    // once in RayGen after traversal returns.
    payload.barycentrics = attributes.barycentrics;
    payload.hitT = RayTCurrent();
    payload.rayConeWidth = RayConeWidthAtDistance(
        payload.rayConeWidth,
        payload.rayConeSpread,
        payload.hitT);
    payload.instanceIndex = InstanceID();
    payload.geometryIndex = GeometryIndex();
    payload.primitiveIndex = PrimitiveIndex();
}
