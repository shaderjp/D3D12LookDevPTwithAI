#ifndef VK_BINDING
#define VK_BINDING(binding, set)
#endif

#ifndef D3D12LOOKDEVPT_WITH_NRD
#define D3D12LOOKDEVPT_WITH_NRD 1
#endif

#if D3D12LOOKDEVPT_WITH_NRD
#include "../ThirdParty/NRD/Shaders/NRD.hlsli"
#else
// Keep the NRD prepare/composite CSO ABI available in dependency-free builds.
// Runtime selection cannot execute these shaders when NRD is disabled, but
// compiling small identity helpers means ThirdParty/NRD may be absent entirely.
float4 NRD_FrontEnd_PackNormalAndRoughness(float3 normal, float roughness, float materialId)
{
    return float4(normal * 0.5f + 0.5f, saturate(roughness));
}

float4 NRD_FrontEnd_UnpackNormalAndRoughness(float4 packed)
{
    return float4(normalize(packed.xyz * 2.0f - 1.0f), packed.w);
}

float REBLUR_FrontEnd_GetNormHitDist(float hitDistance, float viewZ, float3 parameters, float roughness)
{
    float normalization = max(parameters.x + abs(viewZ) * parameters.y + roughness * parameters.z, 1e-6f);
    return hitDistance > 0.0f ? saturate(hitDistance / normalization) : 0.0f;
}

float4 REBLUR_FrontEnd_PackRadianceAndNormHitDist(float3 radiance, float normHitDistance, bool sanitize)
{
    return float4(max(radiance, 0.0f.xxx), saturate(normHitDistance));
}

float4 REBLUR_BackEnd_UnpackRadianceAndNormHitDist(float4 packed)
{
    return packed;
}

float4 RELAX_FrontEnd_PackRadianceAndHitDist(float3 radiance, float hitDistance, bool sanitize)
{
    return float4(max(radiance, 0.0f.xxx), max(hitDistance, 0.0f));
}

void NRD_MaterialFactors(
    float3 normal,
    float3 viewDirection,
    float3 albedo,
    float3 reflectance0,
    float roughness,
    out float3 diffuseFactor,
    out float3 specularFactor)
{
    diffuseFactor = max(albedo, 0.001f.xxx);
    specularFactor = max(reflectance0, 0.001f.xxx);
}
#endif

#include "PathTracingSceneConstants.hlsli"

VK_BINDING(3, 0) ConstantBuffer<SceneConstants> g_scene : register(b0, space0);

VK_BINDING(0, 0) RWTexture2D<float4> g_output : register(u0, space0);
VK_BINDING(13, 0) RWTexture2D<float4> g_denoiseAov0 : register(u5, space0);
VK_BINDING(14, 0) RWTexture2D<float4> g_denoiseAov1 : register(u6, space0);
VK_BINDING(15, 0) RWTexture2D<float4> g_denoiseAov2 : register(u7, space0);
VK_BINDING(19, 0) RWTexture2D<float4> g_previousDenoiseAov0 : register(u11, space0);
VK_BINDING(20, 0) RWTexture2D<float4> g_previousDenoiseAov1 : register(u12, space0);
VK_BINDING(21, 0) RWTexture2D<float4> g_previousDenoiseAov2 : register(u13, space0);
VK_BINDING(25, 0) RWTexture2D<float4> g_signalCurrentRadiance : register(u17, space0);
VK_BINDING(26, 0) RWTexture2D<float4> g_signalDirect : register(u18, space0);
VK_BINDING(27, 0) RWTexture2D<float4> g_signalIndirect : register(u19, space0);
VK_BINDING(28, 0) RWTexture2D<float4> g_signalResidual : register(u20, space0);

VK_BINDING(31, 0) RWTexture2D<float4> g_nrdMotion : register(u23, space0);
VK_BINDING(32, 0) RWTexture2D<float4> g_nrdNormalRoughness : register(u24, space0);
VK_BINDING(33, 0) RWTexture2D<float> g_nrdViewZ : register(u25, space0);
VK_BINDING(34, 0) RWTexture2D<float4> g_nrdDiffRadianceHitDist : register(u26, space0);
VK_BINDING(35, 0) RWTexture2D<float4> g_nrdSpecRadianceHitDist : register(u27, space0);
VK_BINDING(36, 0) RWTexture2D<float4> g_nrdDiffDenoised : register(u28, space0);
VK_BINDING(37, 0) RWTexture2D<float4> g_nrdSpecDenoised : register(u29, space0);
VK_BINDING(41, 0) RWTexture2D<float4> g_postDenoiseHdr : register(u33, space0);
VK_BINDING(44, 0) RWTexture2D<float> g_nrdDiffuseConfidence : register(u36, space0);
VK_BINDING(45, 0) RWTexture2D<float> g_nrdSpecularConfidence : register(u37, space0);
VK_BINDING(46, 0) RWTexture2D<uint> g_surfaceIdentity : register(u38, space0);
VK_BINDING(47, 0) RWTexture2D<uint> g_previousSurfaceIdentity : register(u39, space0);

static const float NrdFp16Max = 65504.0f;
static const float NrdEps = 1e-6f;

bool Invalid3(float3 value)
{
    return any(isnan(value)) || any(isinf(value));
}

bool Invalid1(float value)
{
    return isnan(value) || isinf(value);
}

bool Invalid4(float4 value)
{
    return any(isnan(value)) || any(isinf(value));
}

float3 DecodeNormal(float4 aov)
{
    float3 encoded = aov.xyz * 2.0f - 1.0f;
    return dot(encoded, encoded) > 0.0001f ? normalize(encoded) : float3(0.0f, 1.0f, 0.0f);
}

bool ValidateHistorySurface(
    float4 currentAov0,
    float4 currentAov1,
    float4 currentAov2,
    uint currentIdentity,
    float4 previousAov0,
    float4 previousAov1,
    float4 previousAov2,
    uint previousIdentity)
{
    if (currentAov2.w <= 0.0f || previousAov2.w <= 0.0f || currentAov0.w <= 0.0f || previousAov0.w <= 0.0f)
    {
        return false;
    }
    if (!ValidatePackedSurfaceIdentity(currentIdentity, previousIdentity))
    {
        return false;
    }

    float normalDot = dot(DecodeNormal(currentAov0), DecodeNormal(previousAov0));
    float depthDelta = abs(currentAov0.w - previousAov0.w) / max(currentAov0.w, 1.0f);
    float albedoDelta = length(currentAov1.rgb - previousAov1.rgb);
    float roughnessDelta = abs(currentAov1.w - previousAov1.w);
    return normalDot >= g_scene.validationOptions.x &&
        depthDelta <= g_scene.validationOptions.y &&
        albedoDelta <= g_scene.validationOptions.z &&
        roughnessDelta <= g_scene.validationOptions.w;
}

bool HasValidatedReprojectedHistory(uint2 pixel, uint2 dimensions)
{
    float4 currentAov0 = g_denoiseAov0[pixel];
    float4 currentAov1 = g_denoiseAov1[pixel];
    float4 currentAov2 = g_denoiseAov2[pixel];
    uint currentIdentity = g_surfaceIdentity[pixel];
    uint historyDomains = (uint)round(g_scene.environmentOptions.w);
    if ((historyDomains & (HISTORY_DOMAIN_SURFACE | HISTORY_DOMAIN_DENOISER)) !=
        (HISTORY_DOMAIN_SURFACE | HISTORY_DOMAIN_DENOISER) || currentAov2.w <= 0.5f)
    {
        return false;
    }

    float2 jitterDelta = g_scene.jitterOptions.xy - g_scene.jitterOptions.zw;
    float2 historyPosition = float2(pixel) + currentAov2.xy * float2(dimensions) + jitterDelta;
    int2 basePixel = int2(floor(historyPosition));
    float2 fraction = frac(historyPosition);

    [unroll]
    for (int y = 0; y < 2; ++y)
    {
        [unroll]
        for (int x = 0; x < 2; ++x)
        {
            int2 candidate = basePixel + int2(x, y);
            if (candidate.x < 0 || candidate.y < 0 || candidate.x >= (int)dimensions.x || candidate.y >= (int)dimensions.y)
            {
                continue;
            }

            float tapWeight = (x == 0 ? 1.0f - fraction.x : fraction.x) *
                (y == 0 ? 1.0f - fraction.y : fraction.y);
            if (tapWeight <= 0.0f)
            {
                continue;
            }

            uint2 historyPixel = uint2(candidate);
            if (ValidateHistorySurface(
                currentAov0,
                currentAov1,
                currentAov2,
                currentIdentity,
                g_previousDenoiseAov0[historyPixel],
                g_previousDenoiseAov1[historyPixel],
                g_previousDenoiseAov2[historyPixel],
                g_previousSurfaceIdentity[historyPixel]))
            {
                return true;
            }
        }
    }

    return false;
}

float3 VisualizeLinearViewZ(float viewZ)
{
    float farViewZ = max(g_scene.rayOptions.y, 1.0f);
    return saturate(log2(1.0f + max(viewZ, 0.0f)) / log2(1.0f + farViewZ)).xxx;
}

float3 VisualizeMotion25D(float3 motion, float viewZ)
{
    float relativeViewZMotion = motion.z / max(abs(viewZ), 1.0f);
    return saturate(float3(
        0.5f + motion.x * 20.0f,
        0.5f + motion.y * 20.0f,
        0.5f + relativeViewZMotion * 4.0f));
}

float3 EncodeNormalRoughness101010(float3 normal, float roughness)
{
    normal /= max(abs(normal.x) + abs(normal.y) + abs(normal.z), NrdEps);

    float3 packed;
    packed.y = normal.y * 0.5f + 0.5f;
    packed.x = normal.x * 0.5f + packed.y;
    packed.y -= normal.x * 0.5f;

    roughness = max(saturate(roughness), 1.5f / 512.0f);
    float signedRoughness = normal.z < 0.0f ? -roughness : roughness;
    packed.z = signedRoughness * 0.5f + 0.5f;
    return packed;
}

float4 PackNormalRoughness(float3 normal, float roughness)
{
    return NRD_FrontEnd_PackNormalAndRoughness(normal, roughness, 0.0f);
}

float3 LinearToYCoCg(float3 color)
{
    float y = dot(color, float3(0.25f, 0.5f, 0.25f));
    float co = dot(color, float3(0.5f, 0.0f, -0.5f));
    float cg = dot(color, float3(-0.25f, 0.5f, -0.25f));
    return float3(y, co, cg);
}

float3 YCoCgToLinear(float3 color)
{
    float t = color.x - color.z;
    float3 result;
    result.y = color.x + color.z;
    result.x = t + color.y;
    result.z = t - color.y;
    return max(result, 0.0f.xxx);
}

float SpecMagicCurve(float roughness, float power)
{
    float f = 1.0f - exp2(-200.0f * roughness * roughness);
    return f * pow(saturate(roughness), power);
}

float ReblurHitDistanceNormalization(float viewZ, float roughness)
{
    float smc = SpecMagicCurve(roughness, 0.5f);
    return (3.0f + abs(viewZ) * 0.1f) * lerp(20.0f, 1.0f, smc);
}

float4 PackReblurRadianceHitDistance(float3 radiance, float hitDistance, float viewZ, float roughness)
{
    float normHitDistance = hitDistance > 0.0f
        ? REBLUR_FrontEnd_GetNormHitDist(hitDistance, viewZ, float3(3.0f, 0.1f, 20.0f), roughness)
        : 0.0f;
    return REBLUR_FrontEnd_PackRadianceAndNormHitDist(radiance, normHitDistance, true);
}

float4 PackRelaxRadianceHitDistance(float3 radiance, float hitDistance)
{
    return RELAX_FrontEnd_PackRadianceAndHitDist(radiance, hitDistance, true);
}

float3 ViewDirectionFromPixel(uint2 pixel, uint2 dimensions)
{
    float2 uv = (float2(pixel) + 0.5f.xx + g_scene.jitterOptions.xy) / float2(dimensions);
    float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    float4 nearPoint = mul(float4(ndc, 0.0f, 1.0f), g_scene.inverseViewProjection);
    float4 farPoint = mul(float4(ndc, 1.0f, 1.0f), g_scene.inverseViewProjection);
    nearPoint.xyz /= max(abs(nearPoint.w), NrdEps);
    farPoint.xyz /= max(abs(farPoint.w), NrdEps);
    return normalize(nearPoint.xyz - farPoint.xyz);
}

void MaterialFactors(uint2 pixel, uint2 dimensions, float3 normal, float roughness, float3 albedo, float metallic, out float3 diffuseFactor, out float3 specularFactor)
{
    float3 reflectance0 = lerp(0.04f.xxx, albedo, metallic);
    NRD_MaterialFactors(normal, ViewDirectionFromPixel(pixel, dimensions), albedo * (1.0f - metallic), reflectance0, roughness, diffuseFactor, specularFactor);
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
    if (toneMapper == 1u)
    {
        color = color / (1.0f.xxx + color);
    }
    else if (toneMapper == 2u)
    {
        color = AcesTonemap(color);
    }
    float gamma = max(g_scene.viewOptions.y, 0.01f);
    return pow(saturate(color), 1.0f / gamma);
}

float3 CurrentSignal(uint2 pixel)
{
    return max(g_signalDirect[pixel].rgb, 0.0f.xxx)
        + max(g_signalIndirect[pixel].rgb, 0.0f.xxx)
        + max(g_signalResidual[pixel].rgb, 0.0f.xxx);
}

[numthreads(8, 8, 1)]
void NrdPrepareCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint2 dimensions = (uint2)round(g_scene.rayOptions.zw);
    uint2 pixel = dispatchThreadId.xy;
    if (pixel.x >= dimensions.x || pixel.y >= dimensions.y)
    {
        return;
    }

    float4 aov0 = g_denoiseAov0[pixel];
    float4 aov1 = g_denoiseAov1[pixel];
    float4 aov2 = g_denoiseAov2[pixel];
    bool hit = aov2.w > 0.0f;
    float roughness = saturate(aov1.w);
    float viewZ = hit ? clamp(abs(aov0.w), NrdEps, g_scene.rayOptions.y) : (g_scene.rayOptions.y + 1.0f);
    float3 normal = DecodeNormal(aov0);
    float3 motion = hit ? aov2.xyz : 0.0f.xxx;
    float3 albedo = max(aov1.rgb, 0.0f.xxx);
    float metallic = saturate(g_signalResidual[pixel].a);
    float3 diffuseFactor;
    float3 specularFactor;
    MaterialFactors(pixel, dimensions, normal, roughness, albedo, metallic, diffuseFactor, specularFactor);
    // The path tracer performs the probabilistic primary-lobe split. These
    // are matched estimators, not a post-hoc material-energy partition: alpha
    // carries the actual first secondary hit distance for the sampled lobe.
    float3 diffuse = hit ? max(g_signalDirect[pixel].rgb, 0.0f.xxx) / max(diffuseFactor, 0.02f.xxx) : 0.0f.xxx;
    float3 specular = hit ? max(g_signalIndirect[pixel].rgb, 0.0f.xxx) / max(specularFactor, 0.02f.xxx) : 0.0f.xxx;
    float diffuseHitDistance = hit ? max(g_signalDirect[pixel].a, 0.0f) : 0.0f;
    float specularHitDistance = hit ? max(g_signalIndirect[pixel].a, 0.0f) : 0.0f;
    bool relax = g_scene.denoisePassOptions.x > 0.5f;

    bool reprojectedHistory = hit && HasValidatedReprojectedHistory(pixel, dimensions);
    float surfaceConfidence = reprojectedHistory ? 1.0f : 0.0f;
    float diffuseSampleConfidence = diffuseHitDistance > 0.0f ? 1.0f : 0.25f;
    float specularSampleConfidence = specularHitDistance > 0.0f ? 1.0f : 0.15f;
    // Low-roughness specular history is deliberately less trusted because a
    // sub-pixel normal/motion error produces a large reflected-direction error.
    specularSampleConfidence *= lerp(0.55f, 1.0f, roughness);

    g_nrdMotion[pixel] = float4(motion, 0.0f);
    g_nrdNormalRoughness[pixel] = PackNormalRoughness(normal, roughness);
    g_nrdViewZ[pixel] = viewZ;
    g_nrdDiffuseConfidence[pixel] = saturate(surfaceConfidence * diffuseSampleConfidence);
    g_nrdSpecularConfidence[pixel] = saturate(surfaceConfidence * specularSampleConfidence);
    g_nrdDiffRadianceHitDist[pixel] = relax
        ? PackRelaxRadianceHitDistance(diffuse, diffuseHitDistance)
        : PackReblurRadianceHitDistance(diffuse, diffuseHitDistance, viewZ, 1.0f);
    g_nrdSpecRadianceHitDist[pixel] = relax
        ? PackRelaxRadianceHitDistance(specular, specularHitDistance)
        : PackReblurRadianceHitDistance(specular, specularHitDistance, viewZ, roughness);
}

[numthreads(8, 8, 1)]
void NrdCompositeCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint2 dimensions = (uint2)round(g_scene.rayOptions.zw);
    uint2 pixel = dispatchThreadId.xy;
    if (pixel.x >= dimensions.x || pixel.y >= dimensions.y)
    {
        return;
    }

    bool hit = g_denoiseAov2[pixel].w > 0.0f;
    bool relax = g_scene.denoisePassOptions.x > 0.5f;
    float3 currentColor = CurrentSignal(pixel);
    float4 diffData = g_nrdDiffDenoised[pixel];
    float4 specData = g_nrdSpecDenoised[pixel];
    float3 diffuse = relax ? max(diffData.rgb, 0.0f.xxx) : max(REBLUR_BackEnd_UnpackRadianceAndNormHitDist(diffData).rgb, 0.0f.xxx);
    float3 specular = relax ? max(specData.rgb, 0.0f.xxx) : max(REBLUR_BackEnd_UnpackRadianceAndNormHitDist(specData).rgb, 0.0f.xxx);
    float4 aov0 = g_denoiseAov0[pixel];
    float4 aov1 = g_denoiseAov1[pixel];
    float3 normal = DecodeNormal(aov0);
    float roughness = saturate(aov1.w);
    float metallic = saturate(g_signalResidual[pixel].a);
    float3 diffuseFactor;
    float3 specularFactor;
    MaterialFactors(pixel, dimensions, normal, roughness, max(aov1.rgb, 0.0f.xxx), metallic, diffuseFactor, specularFactor);
    float3 emission = max(g_signalResidual[pixel].rgb, 0.0f.xxx);
    float3 filtered = hit ? max(diffuse * diffuseFactor + specular * specularFactor + emission, 0.0f.xxx) : currentColor;

    uint debugMode = (uint)round(g_scene.debugOptions.x);
    if (debugMode == 16u) filtered = currentColor;
    if (debugMode == 20u) filtered = float3(0.5f + g_denoiseAov2[pixel].x * 20.0f, 0.5f + g_denoiseAov2[pixel].y * 20.0f, 0.5f);
    if (debugMode == 32u) filtered = max(g_signalDirect[pixel].rgb, 0.0f.xxx);
    if (debugMode == 33u) filtered = max(g_signalIndirect[pixel].rgb, 0.0f.xxx);
    if (debugMode == 34u) filtered = max(g_signalResidual[pixel].rgb, 0.0f.xxx);
    if (debugMode == 35u) filtered = currentColor;
    if (debugMode == 36u) filtered = diffuse;
    if (debugMode == 37u) filtered = specular;
    if (debugMode == 41u)
    {
        float4 normalRoughness = NRD_FrontEnd_UnpackNormalAndRoughness(g_nrdNormalRoughness[pixel]);
        filtered = normalRoughness.xyz * 0.5f + 0.5f;
    }
    if (debugMode == 42u)
    {
        float4 normalRoughness = NRD_FrontEnd_UnpackNormalAndRoughness(g_nrdNormalRoughness[pixel]);
        filtered = saturate(normalRoughness.w).xxx;
    }
    if (debugMode == 43u)
    {
        filtered = VisualizeLinearViewZ(g_nrdViewZ[pixel]);
    }
    if (debugMode == 44u)
    {
        filtered = VisualizeMotion25D(g_nrdMotion[pixel].xyz, g_nrdViewZ[pixel]);
    }
    if (debugMode == 45u)
    {
        // Red = rejected/disoccluded surface, green = accepted reprojected
        // surface history, blue = background/no primary surface.
        filtered = hit
            ? (HasValidatedReprojectedHistory(pixel, dimensions) ? float3(0.0f, 1.0f, 0.0f) : float3(1.0f, 0.0f, 0.0f))
            : float3(0.0f, 0.0f, 1.0f);
    }
    if (debugMode == 46u)
    {
        float4 normalRoughness = NRD_FrontEnd_UnpackNormalAndRoughness(g_nrdNormalRoughness[pixel]);
        bool nonFinite =
            Invalid4(g_denoiseAov0[pixel]) ||
            Invalid4(g_denoiseAov1[pixel]) ||
            Invalid4(g_denoiseAov2[pixel]) ||
            Invalid4(g_signalDirect[pixel]) ||
            Invalid4(g_signalIndirect[pixel]) ||
            Invalid4(g_signalResidual[pixel]) ||
            Invalid4(g_nrdMotion[pixel]) ||
            Invalid4(g_nrdNormalRoughness[pixel]) ||
            Invalid1(g_nrdViewZ[pixel]) ||
            Invalid4(g_nrdDiffRadianceHitDist[pixel]) ||
            Invalid4(g_nrdSpecRadianceHitDist[pixel]) ||
            Invalid4(normalRoughness);
        float normalLength = length(normalRoughness.xyz);
        bool semanticallyInvalid = hit &&
            (g_nrdViewZ[pixel] <= 0.0f || normalLength < 0.95f || normalLength > 1.05f ||
                normalRoughness.w < 0.0f || normalRoughness.w > 1.0f);
        // Green = finite and contract-valid, yellow = finite but outside the
        // input contract, magenta = NaN or Inf in any NRD input.
        filtered = nonFinite
            ? float3(1.0f, 0.0f, 1.0f)
            : (semanticallyInvalid ? float3(1.0f, 1.0f, 0.0f) : float3(0.0f, 1.0f, 0.0f));
    }

    if (Invalid3(filtered))
    {
        filtered = currentColor;
    }

    g_postDenoiseHdr[pixel] = float4(max(filtered, 0.0f.xxx), 1.0f);
    if (g_scene.denoisePassOptions.z < 0.5f)
    {
        g_output[pixel] = float4(Tonemap(filtered), 1.0f);
    }
}
