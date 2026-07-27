#if defined(VULKAN)
#define VK_BINDING(slot, descriptorSet) [[vk::binding(slot, descriptorSet)]]
#else
#define VK_BINDING(binding, set)
#endif

#include "PathTracingSceneConstants.hlsli"

// ABI-compatible with RTXDI_PackedDIReservoir (24 bytes). Debug views inspect
// identity/weight/age only; radiance is never stored in the reservoir.
struct RestirReservoir
{
    uint lightData;
    uint uvData;
    uint mVisibility;
    uint distanceAge;
    float targetPdf;
    float weight;
};

VK_BINDING(1, 0) RWTexture2D<float4> g_output : register(u0, space0);
VK_BINDING(2, 0) RWTexture2D<float4> g_accumulation : register(u1, space0);
VK_BINDING(3, 0) ConstantBuffer<SceneConstants> g_scene : register(b0, space0);
VK_BINDING(10, 0) RWStructuredBuffer<RestirReservoir> g_restirHistory : register(u3, space0);
VK_BINDING(13, 0) RWTexture2D<float4> g_denoiseAov0 : register(u5, space0);
VK_BINDING(14, 0) RWTexture2D<float4> g_denoiseAov1 : register(u6, space0);
VK_BINDING(15, 0) RWTexture2D<float4> g_denoiseAov2 : register(u7, space0);
// Internal fallback storage deliberately aliases resources that are idle while
// NRD is not executing. u26-u29 are persistent lobe histories; the legacy
// radiance A/B and denoise ping/pong resources are the two spatial ping-pong
// pairs. Moments and history controls remain ping-ponged at u9/u31 and u10/u32.
VK_BINDING(16, 0) RWTexture2D<float4> g_diffuseSpatialPing : register(u8, space0);
VK_BINDING(17, 0) RWTexture2D<float4> g_reconstructionHistoryMoments : register(u9, space0);
VK_BINDING(18, 0) RWTexture2D<float4> g_reconstructionHistoryLength : register(u10, space0);
VK_BINDING(19, 0) RWTexture2D<float4> g_previousDenoiseAov0 : register(u11, space0);
VK_BINDING(20, 0) RWTexture2D<float4> g_previousDenoiseAov1 : register(u12, space0);
VK_BINDING(21, 0) RWTexture2D<float4> g_previousDenoiseAov2 : register(u13, space0);
VK_BINDING(25, 0) RWTexture2D<float4> g_signalCurrentRadiance : register(u17, space0);
VK_BINDING(26, 0) RWTexture2D<float4> g_signalDirect : register(u18, space0);
VK_BINDING(27, 0) RWTexture2D<float4> g_signalIndirect : register(u19, space0);
VK_BINDING(28, 0) RWTexture2D<float4> g_signalResidual : register(u20, space0);
VK_BINDING(29, 0) RWTexture2D<float4> g_specularSpatialPing : register(u21, space0);
VK_BINDING(30, 0) RWTexture2D<float4> g_specularSpatialPong : register(u22, space0);
VK_BINDING(34, 0) RWTexture2D<float4> g_diffuseHistoryA : register(u26, space0);
VK_BINDING(35, 0) RWTexture2D<float4> g_specularHistoryA : register(u27, space0);
VK_BINDING(36, 0) RWTexture2D<float4> g_diffuseHistoryB : register(u28, space0);
VK_BINDING(37, 0) RWTexture2D<float4> g_specularHistoryB : register(u29, space0);
VK_BINDING(38, 0) RWTexture2D<float4> g_diffuseSpatialPong : register(u30, space0);
VK_BINDING(39, 0) RWTexture2D<float4> g_reconstructionHistoryMomentsB : register(u31, space0);
VK_BINDING(40, 0) RWTexture2D<float4> g_reconstructionHistoryLengthB : register(u32, space0);
VK_BINDING(41, 0) RWTexture2D<float4> g_postDenoiseHdr : register(u33, space0);
VK_BINDING(44, 0) RWTexture2D<float> g_diffuseHistoryConfidence : register(u36, space0);
VK_BINDING(45, 0) RWTexture2D<float> g_specularHistoryConfidence : register(u37, space0);
VK_BINDING(46, 0) RWTexture2D<uint> g_surfaceIdentity : register(u38, space0);
VK_BINDING(47, 0) RWTexture2D<uint> g_previousSurfaceIdentity : register(u39, space0);

float Luminance(float3 color)
{
    return dot(color, float3(0.2126f, 0.7152f, 0.0722f));
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

float3 DecodeNormal(float4 aov)
{
    float3 encoded = aov.xyz * 2.0f - 1.0f;
    return dot(encoded, encoded) > 0.0001f ? normalize(encoded) : float3(0.0f, 1.0f, 0.0f);
}

bool Invalid4(float4 value)
{
    return any(isnan(value)) || any(isinf(value));
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

float4 PackHistoryControls(float4 controls)
{
    return float4(
        saturate(round(max(controls.xy, 0.0f.xx)) / HistoryLengthUnormScale),
        saturate(controls.zw));
}

float4 LoadPreviousDiffuseHistory(uint2 pixel)
{
    return PreviousHistoryIsA() ? g_diffuseHistoryA[pixel] : g_diffuseHistoryB[pixel];
}

float4 LoadPreviousSpecularHistory(uint2 pixel)
{
    return PreviousHistoryIsA() ? g_specularHistoryA[pixel] : g_specularHistoryB[pixel];
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

float4 LoadCurrentDiffuseHistory(uint2 pixel)
{
    return PreviousHistoryIsA() ? g_diffuseHistoryB[pixel] : g_diffuseHistoryA[pixel];
}

float4 LoadCurrentSpecularHistory(uint2 pixel)
{
    return PreviousHistoryIsA() ? g_specularHistoryB[pixel] : g_specularHistoryA[pixel];
}

float4 LoadCurrentHistoryMoments(uint2 pixel)
{
    return PreviousHistoryIsA() ? g_reconstructionHistoryMomentsB[pixel] : g_reconstructionHistoryMoments[pixel];
}

float4 LoadCurrentHistoryLength(uint2 pixel)
{
    float4 packedControls = PreviousHistoryIsA()
        ? g_reconstructionHistoryLengthB[pixel]
        : g_reconstructionHistoryLength[pixel];
    return UnpackHistoryControls(packedControls);
}

void StoreCurrentHistories(uint2 pixel, float4 diffuse, float4 specular, float4 moments, float4 lengthData)
{
    float4 packedLengthData = PackHistoryControls(lengthData);
    if (PreviousHistoryIsA())
    {
        g_diffuseHistoryB[pixel] = diffuse;
        g_specularHistoryB[pixel] = specular;
        g_reconstructionHistoryMomentsB[pixel] = moments;
        g_reconstructionHistoryLengthB[pixel] = packedLengthData;
    }
    else
    {
        g_diffuseHistoryA[pixel] = diffuse;
        g_specularHistoryA[pixel] = specular;
        g_reconstructionHistoryMoments[pixel] = moments;
        g_reconstructionHistoryLength[pixel] = packedLengthData;
    }
}

float KernelWeight(int offset)
{
    int a = abs(offset);
    return a == 0 ? 6.0f : (a == 1 ? 4.0f : 1.0f);
}

uint PixelIndex(uint2 pixel, uint2 dimensions)
{
    return pixel.y * dimensions.x + pixel.x;
}

uint RtxdiReservoirPointer(uint2 pixel, uint2 dimensions)
{
    static const uint BlockSize = 16u;
    uint blockRowPitch = ((dimensions.x + BlockSize - 1u) / BlockSize) * BlockSize * BlockSize;
    uint2 block = pixel / BlockSize;
    uint2 local = pixel % BlockSize;
    return block.y * blockRowPitch + block.x * BlockSize * BlockSize + local.y * BlockSize + local.x;
}

bool IsInside(int2 pixel, uint2 dimensions)
{
    return pixel.x >= 0 && pixel.y >= 0 && pixel.x < (int)dimensions.x && pixel.y < (int)dimensions.y;
}

bool ValidateAov(float4 currentAov0, float4 currentAov1, float4 currentAov2, float4 historyAov0, float4 historyAov1, float4 historyAov2)
{
    if (currentAov2.w <= 0.5f || historyAov2.w <= 0.5f || currentAov0.w <= 0.0f || historyAov0.w <= 0.0f)
    {
        return false;
    }

    float normalDot = dot(DecodeNormal(currentAov0), DecodeNormal(historyAov0));
    float depthDelta = abs(currentAov0.w - historyAov0.w) / max(currentAov0.w, 1.0f);
    float albedoDelta = length(currentAov1.rgb - historyAov1.rgb);
    float roughnessDelta = abs(currentAov1.w - historyAov1.w);
    return normalDot >= g_scene.validationOptions.x &&
        depthDelta <= g_scene.validationOptions.y &&
        albedoDelta <= g_scene.validationOptions.z &&
        roughnessDelta <= g_scene.validationOptions.w;
}

bool ValidateTemporalAov(
    float4 currentAov0,
    float4 currentAov1,
    float4 currentAov2,
    uint currentIdentity,
    float4 historyAov0,
    float4 historyAov1,
    float4 historyAov2,
    uint historyIdentity)
{
    return ValidatePackedSurfaceIdentity(currentIdentity, historyIdentity) &&
        ValidateAov(currentAov0, currentAov1, currentAov2, historyAov0, historyAov1, historyAov2);
}

float AovMatchScore(float4 currentAov0, float4 currentAov1, float4 currentAov2, float4 historyAov0, float4 historyAov1, float4 historyAov2)
{
    float normalError = 1.0f - saturate(dot(DecodeNormal(currentAov0), DecodeNormal(historyAov0)));
    float depthError = abs(currentAov0.w - historyAov0.w) / max(currentAov0.w, 1.0f);
    float albedoError = length(currentAov1.rgb - historyAov1.rgb);
    float roughnessError = abs(currentAov1.w - historyAov1.w);
    return normalError * 2.0f + depthError * 8.0f + albedoError + roughnessError;
}

bool UseSplitSignals()
{
    return g_scene.signalDenoiseOptions.x >= 0.5f;
}

float4 CurrentDiffuseSignal(uint2 pixel)
{
    if (!UseSplitSignals())
    {
        float3 combined = max(g_signalDirect[pixel].rgb, 0.0f.xxx)
            + max(g_signalIndirect[pixel].rgb, 0.0f.xxx)
            + max(g_signalResidual[pixel].rgb, 0.0f.xxx);
        return float4(combined, 0.0f);
    }
    float4 signal = g_signalDirect[pixel];
    return float4(max(signal.rgb, 0.0f.xxx), max(signal.a, 0.0f));
}

float4 CurrentSpecularSignal(uint2 pixel)
{
    if (!UseSplitSignals())
    {
        return 0.0f.xxxx;
    }
    float4 signal = g_signalIndirect[pixel];
    return float4(max(signal.rgb, 0.0f.xxx), max(signal.a, 0.0f));
}

float3 CurrentResidualSignal(uint2 pixel)
{
    return UseSplitSignals() ? max(g_signalResidual[pixel].rgb, 0.0f.xxx) : 0.0f.xxx;
}

float3 CurrentSignal(uint2 pixel)
{
    return CurrentDiffuseSignal(pixel).rgb + CurrentSpecularSignal(pixel).rgb + CurrentResidualSignal(pixel);
}

float4 CurrentLobeSignal(uint2 pixel, bool specular)
{
    return specular ? CurrentSpecularSignal(pixel) : CurrentDiffuseSignal(pixel);
}

bool FindHistoryPixel(
    uint2 pixel,
    uint2 dimensions,
    float4 centerAov0,
    float4 centerAov1,
    float4 centerAov2,
    uint centerIdentity,
    out int2 bestPixel,
    out float bestScore)
{
    float2 motion = centerAov2.xy;
    float2 jitterDelta = g_scene.jitterOptions.xy - g_scene.jitterOptions.zw;
    int2 basePixel = int2(round(float2(pixel) + motion * float2(dimensions) + jitterDelta));
    bestPixel = basePixel;
    bestScore = 1e20f;

    uint historyDomains = (uint)round(g_scene.environmentOptions.w);
    if ((historyDomains & (HISTORY_DOMAIN_SURFACE | HISTORY_DOMAIN_DENOISER)) !=
        (HISTORY_DOMAIN_SURFACE | HISTORY_DOMAIN_DENOISER))
    {
        return false;
    }

    bool found = false;
    [unroll]
    for (int y = -1; y <= 1; ++y)
    {
        [unroll]
        for (int x = -1; x <= 1; ++x)
        {
            int2 candidate = basePixel + int2(x, y);
            if (!IsInside(candidate, dimensions))
            {
                continue;
            }

            float4 historyAov0 = g_previousDenoiseAov0[uint2(candidate)];
            float4 historyAov1 = g_previousDenoiseAov1[uint2(candidate)];
            float4 historyAov2 = g_previousDenoiseAov2[uint2(candidate)];
            if (!ValidateTemporalAov(
                centerAov0,
                centerAov1,
                centerAov2,
                centerIdentity,
                historyAov0,
                historyAov1,
                historyAov2,
                g_previousSurfaceIdentity[uint2(candidate)]))
            {
                continue;
            }

            float score = AovMatchScore(centerAov0, centerAov1, centerAov2, historyAov0, historyAov1, historyAov2);
            if (score < bestScore)
            {
                bestScore = score;
                bestPixel = candidate;
                found = true;
            }
        }
    }

    return found;
}

struct TemporalHistorySample
{
    float4 diffuse;
    float4 specular;
    float4 moments;
    float4 lengthData;
    float4 aov0;
    float4 aov1;
    float4 aov2;
    float score;
};

bool GatherTemporalHistory(
    uint2 pixel,
    uint2 dimensions,
    float4 centerAov0,
    float4 centerAov1,
    float4 centerAov2,
    uint centerIdentity,
    out TemporalHistorySample history)
{
    history.diffuse = 0.0f.xxxx;
    history.specular = 0.0f.xxxx;
    history.moments = 0.0f.xxxx;
    history.lengthData = 0.0f.xxxx;
    history.aov0 = 0.0f.xxxx;
    history.aov1 = 0.0f.xxxx;
    history.aov2 = 0.0f.xxxx;
    history.score = 0.0f;

    uint historyDomains = (uint)round(g_scene.environmentOptions.w);
    if ((historyDomains & (HISTORY_DOMAIN_SURFACE | HISTORY_DOMAIN_DENOISER)) !=
        (HISTORY_DOMAIN_SURFACE | HISTORY_DOMAIN_DENOISER))
    {
        return false;
    }

    float2 jitterDelta = g_scene.jitterOptions.xy - g_scene.jitterOptions.zw;
    float2 historyPosition = float2(pixel) + centerAov2.xy * float2(dimensions) + jitterDelta;
    int2 basePixel = int2(floor(historyPosition));
    float2 fraction = frac(historyPosition);
    float totalWeight = 0.0f;

    [unroll]
    for (int y = 0; y < 2; ++y)
    {
        [unroll]
        for (int x = 0; x < 2; ++x)
        {
            int2 candidate = basePixel + int2(x, y);
            if (!IsInside(candidate, dimensions))
            {
                continue;
            }

            uint2 candidatePixel = uint2(candidate);
            float4 candidateAov0 = g_previousDenoiseAov0[candidatePixel];
            float4 candidateAov1 = g_previousDenoiseAov1[candidatePixel];
            float4 candidateAov2 = g_previousDenoiseAov2[candidatePixel];
            if (!ValidateTemporalAov(
                centerAov0,
                centerAov1,
                centerAov2,
                centerIdentity,
                candidateAov0,
                candidateAov1,
                candidateAov2,
                g_previousSurfaceIdentity[candidatePixel]))
            {
                continue;
            }

            float weightX = x == 0 ? 1.0f - fraction.x : fraction.x;
            float weightY = y == 0 ? 1.0f - fraction.y : fraction.y;
            float weight = weightX * weightY;
            if (weight <= 0.0f)
            {
                continue;
            }

            history.diffuse += max(LoadPreviousDiffuseHistory(candidatePixel), 0.0f.xxxx) * weight;
            history.specular += max(LoadPreviousSpecularHistory(candidatePixel), 0.0f.xxxx) * weight;
            history.moments += LoadPreviousHistoryMoments(candidatePixel) * weight;
            history.lengthData += LoadPreviousHistoryLength(candidatePixel) * weight;
            history.aov0 += candidateAov0 * weight;
            history.aov1 += candidateAov1 * weight;
            history.aov2 += candidateAov2 * weight;
            history.score += AovMatchScore(centerAov0, centerAov1, centerAov2, candidateAov0, candidateAov1, candidateAov2) * weight;
            totalWeight += weight;
        }
    }

    if (totalWeight > 0.0001f)
    {
        float inverseWeight = rcp(totalWeight);
        history.diffuse *= inverseWeight;
        history.specular *= inverseWeight;
        history.moments *= inverseWeight;
        history.lengthData *= inverseWeight;
        history.aov0 *= inverseWeight;
        history.aov1 *= inverseWeight;
        history.aov2 *= inverseWeight;
        history.score *= inverseWeight;
        return true;
    }

    int2 fallbackPixel;
    float fallbackScore;
    if (!FindHistoryPixel(pixel, dimensions, centerAov0, centerAov1, centerAov2, centerIdentity, fallbackPixel, fallbackScore))
    {
        return false;
    }

    uint2 fallback = uint2(fallbackPixel);
    history.diffuse = max(LoadPreviousDiffuseHistory(fallback), 0.0f.xxxx);
    history.specular = max(LoadPreviousSpecularHistory(fallback), 0.0f.xxxx);
    history.moments = LoadPreviousHistoryMoments(fallback);
    history.lengthData = LoadPreviousHistoryLength(fallback);
    history.aov0 = g_previousDenoiseAov0[fallback];
    history.aov1 = g_previousDenoiseAov1[fallback];
    history.aov2 = g_previousDenoiseAov2[fallback];
    history.score = fallbackScore;
    return true;
}

void CurrentNeighborhoodBounds(uint2 pixel, uint2 dimensions, bool specular, out float3 lowValue, out float3 highValue)
{
    float3 center = CurrentLobeSignal(pixel, specular).rgb;
    lowValue = center;
    highValue = center;
    int2 centerPixel = int2(pixel);

    [unroll]
    for (int y = -1; y <= 1; ++y)
    {
        [unroll]
        for (int x = -1; x <= 1; ++x)
        {
            int2 samplePixel = clamp(centerPixel + int2(x, y), int2(0, 0), int2((int)dimensions.x, (int)dimensions.y) - int2(1, 1));
            float3 sampleColor = CurrentLobeSignal(uint2(samplePixel), specular).rgb;
            lowValue = min(lowValue, sampleColor);
            highValue = max(highValue, sampleColor);
        }
    }
}

float3 ClampHistoryToNeighborhood(uint2 pixel, uint2 dimensions, bool specular, float3 history)
{
    if (g_scene.stabilityOptions.x < 0.5f)
    {
        return history;
    }

    float3 lowValue;
    float3 highValue;
    CurrentNeighborhoodBounds(pixel, dimensions, specular, lowValue, highValue);
    float3 extent = max(highValue - lowValue, 0.03f.xxx);
    return clamp(history, lowValue - extent * 0.35f, highValue + extent * 0.35f);
}

float3 ClipCurrentFirefly(uint2 pixel, uint2 dimensions, bool specular, float3 currentColor, float4 centerAov0, float4 centerAov1, float4 centerAov2)
{
    if (centerAov0.w <= 0.0f || centerAov2.w <= 0.5f)
    {
        return currentColor;
    }

    float centerLum = Luminance(currentColor);
    if (centerLum <= 0.0f)
    {
        return currentColor;
    }

    float sumLum = 0.0f;
    float sumLum2 = 0.0f;
    float maxNeighborLum = 0.0f;
    float neighborCount = 0.0f;
    int2 centerPixel = int2(pixel);

    [unroll]
    for (int y = -1; y <= 1; ++y)
    {
        [unroll]
        for (int x = -1; x <= 1; ++x)
        {
            if (x == 0 && y == 0)
            {
                continue;
            }

            int2 samplePixel = clamp(centerPixel + int2(x, y), int2(0, 0), int2((int)dimensions.x, (int)dimensions.y) - int2(1, 1));
            float4 sampleAov0 = g_denoiseAov0[uint2(samplePixel)];
            float4 sampleAov1 = g_denoiseAov1[uint2(samplePixel)];
            float4 sampleAov2 = g_denoiseAov2[uint2(samplePixel)];
            if (!ValidateAov(centerAov0, centerAov1, centerAov2, sampleAov0, sampleAov1, sampleAov2))
            {
                continue;
            }

            float sampleLum = Luminance(CurrentLobeSignal(uint2(samplePixel), specular).rgb);
            sumLum += sampleLum;
            sumLum2 += sampleLum * sampleLum;
            maxNeighborLum = max(maxNeighborLum, sampleLum);
            neighborCount += 1.0f;
        }
    }

    if (neighborCount < 3.0f)
    {
        return currentColor;
    }

    float meanLum = sumLum / neighborCount;
    float variance = max(sumLum2 / neighborCount - meanLum * meanLum, 0.0f);
    float roughnessAllowance = lerp(1.0f, 1.35f, saturate(centerAov1.w));
    float threshold = max(meanLum + sqrt(variance) * 4.0f, maxNeighborLum * 1.5f);
    threshold = max(threshold * roughnessAllowance, g_scene.denoiseOptions2.x);
    if (centerLum <= threshold)
    {
        return currentColor;
    }

    return currentColor * (threshold / max(centerLum, 0.0001f));
}

float ComputeHistoryConfidence(bool validHistory, float historyScore, float4 currentAov0, float4 currentAov1, float4 currentAov2,
    float4 historyAov0, float4 historyAov1, float4 historyAov2, float currentLum, float previousLum)
{
    if (!validHistory)
    {
        return 0.0f;
    }

    float normalDot = dot(DecodeNormal(currentAov0), DecodeNormal(historyAov0));
    float normalConfidence = saturate((normalDot - g_scene.validationOptions.x) / max(1.0f - g_scene.validationOptions.x, 0.001f));
    float depthDelta = abs(currentAov0.w - historyAov0.w) / max(currentAov0.w, 1.0f);
    float depthConfidence = saturate(1.0f - depthDelta / max(g_scene.validationOptions.y, 0.001f));
    float albedoDelta = length(currentAov1.rgb - historyAov1.rgb);
    float albedoConfidence = saturate(1.0f - albedoDelta / max(g_scene.validationOptions.z, 0.001f));
    float roughnessDelta = abs(currentAov1.w - historyAov1.w);
    float roughnessConfidence = saturate(1.0f - roughnessDelta / max(g_scene.validationOptions.w, 0.001f));
    float motionConfidence = saturate(1.0f - length(currentAov2.xy) * 32.0f);
    float luminanceDelta = abs(currentLum - previousLum) / max(max(currentLum, previousLum), 1.0f);
    float luminanceConfidence = saturate(1.0f - luminanceDelta / max(g_scene.signalDenoiseOptions.z, 0.05f));
    float scoreConfidence = saturate(1.0f - historyScore * 0.25f);
    return normalConfidence * depthConfidence * albedoConfidence * roughnessConfidence * motionConfidence * luminanceConfidence * scoreConfidence;
}

float3 ApplyHistoryClamp(float3 current, float3 history, float variance, float historyLength)
{
    if (historyLength <= 0.0f)
    {
        return current;
    }

    float sigma = sqrt(max(variance, 0.0f));
    float clampSigma = max(g_scene.signalDenoiseOptions.y, 0.01f);
    float3 radius = max((sigma * clampSigma).xxx, max(abs(history) * 0.12f, 0.05f.xxx));
    return clamp(current, history - radius, history + radius);
}

float4 LoadAtrousInput(uint2 pixel, uint passIndex, bool specular)
{
    if (specular)
    {
        return (passIndex & 1u) == 0u ? g_specularSpatialPing[pixel] : g_specularSpatialPong[pixel];
    }
    return (passIndex & 1u) == 0u ? g_diffuseSpatialPing[pixel] : g_diffuseSpatialPong[pixel];
}

void StoreAtrousOutput(uint2 pixel, uint passIndex, bool specular, float4 value)
{
    if (specular)
    {
        if ((passIndex & 1u) == 0u)
        {
            g_specularSpatialPong[pixel] = value;
        }
        else
        {
            g_specularSpatialPing[pixel] = value;
        }
    }
    else
    {
        if ((passIndex & 1u) == 0u)
        {
            g_diffuseSpatialPong[pixel] = value;
        }
        else
        {
            g_diffuseSpatialPing[pixel] = value;
        }
    }
}

float4 LoadFinalAtrous(uint2 pixel, bool specular)
{
    uint passCount = min((uint)round(g_scene.atrousOptions.x), 5u);
    if (passCount == 0u)
    {
        return specular ? g_specularSpatialPing[pixel] : g_diffuseSpatialPing[pixel];
    }
    if (specular)
    {
        return (passCount & 1u) == 0u ? g_specularSpatialPing[pixel] : g_specularSpatialPong[pixel];
    }
    return (passCount & 1u) == 0u ? g_diffuseSpatialPing[pixel] : g_diffuseSpatialPong[pixel];
}

struct TemporalLobeResult
{
    float4 signal;
    float mean;
    float mean2;
    float historyLength;
    float reactive;
    float confidence;
};

float ResolveTemporalHitDistance(float currentHitDistance, float historyHitDistance, bool validHistory, float alpha)
{
    bool currentValid = currentHitDistance > 0.0f;
    bool historyValid = validHistory && historyHitDistance > 0.0f;
    if (currentValid && historyValid)
    {
        return lerp(historyHitDistance, currentHitDistance, max(saturate(alpha), 0.2f));
    }
    return currentValid ? currentHitDistance : (historyValid ? historyHitDistance : 0.0f);
}

TemporalLobeResult ResolveTemporalLobe(
    uint2 pixel,
    uint2 dimensions,
    bool specular,
    float4 currentSignal,
    float4 historySignal,
    bool validSurfaceHistory,
    float historyScore,
    float previousMean,
    float previousMean2,
    float previousHistoryLength,
    float previousReactive,
    float4 centerAov0,
    float4 centerAov1,
    float4 centerAov2,
    float4 historyAov0,
    float4 historyAov1,
    float4 historyAov2)
{
    TemporalLobeResult result;
    float3 currentColor = ClipCurrentFirefly(
        pixel, dimensions, specular, currentSignal.rgb, centerAov0, centerAov1, centerAov2);
    float currentLum = Luminance(currentColor);
    bool validHistory = validSurfaceHistory && previousHistoryLength > 0.0f && !Invalid4(historySignal);
    float historyLength = validHistory ? previousHistoryLength : 0.0f;
    float3 reprojectedHistory = validHistory ? max(historySignal.rgb, 0.0f.xxx) : currentColor;
    float previousLum = Luminance(reprojectedHistory);
    float previousVariance = validHistory ? max(previousMean2 - previousMean * previousMean, 0.0f) : currentLum * currentLum;
    float historyConfidence = ComputeHistoryConfidence(
        validHistory,
        historyScore,
        centerAov0,
        centerAov1,
        centerAov2,
        historyAov0,
        historyAov1,
        historyAov2,
        currentLum,
        previousLum);
    float luminanceDelta = abs(currentLum - previousLum);
    float reactiveThreshold = max(previousLum, 1.0f) * g_scene.signalDenoiseOptions.z;
    float currentReactive = (!validHistory || historyConfidence < 0.25f || luminanceDelta > reactiveThreshold) ? 1.0f : 0.0f;
    float reactive = max(currentReactive, validHistory ? previousReactive * 0.5f : 0.0f);

    float roughness = saturate(centerAov1.w);
    float maxHistoryFrames = max(g_scene.reconstructionOptions.y, 1.0f);
    if (specular)
    {
        float specularHistoryScale = saturate(g_scene.signalDenoiseOptions.w);
        maxHistoryFrames = max(maxHistoryFrames * lerp(max(specularHistoryScale, 0.05f), 1.0f, roughness), 1.0f);
    }
    float historyFactor = saturate(historyLength / maxHistoryFrames);
    float alpha = lerp(g_scene.reconstructionOptions.w, g_scene.reconstructionOptions.z, historyFactor);
    alpha = lerp(alpha, max(alpha, 0.65f), reactive);
    if (specular)
    {
        alpha = lerp(max(alpha, g_scene.signalDenoiseOptions.w), alpha, roughness);
    }
    alpha = lerp(max(alpha, 0.85f), alpha, historyConfidence);
    alpha = max(alpha, saturate(g_scene.stabilityOptions.y * 0.55f));

    if (g_scene.reconstructionOptions.x < 0.5f)
    {
        alpha = 1.0f;
        historyLength = 0.0f;
        reactive = 1.0f;
        historyConfidence = 0.0f;
        validHistory = false;
    }

    float3 clampedHistory = ClampHistoryToNeighborhood(pixel, dimensions, specular, reprojectedHistory);
    float3 clampedCurrent = ApplyHistoryClamp(currentColor, clampedHistory, previousVariance, historyLength);
    float3 temporal = lerp(clampedHistory, clampedCurrent, saturate(alpha));
    float momentAlpha = historyLength > 0.0f ? saturate(alpha) : 1.0f;
    result.mean = lerp(validHistory ? previousMean : currentLum, currentLum, momentAlpha);
    result.mean2 = lerp(validHistory ? previousMean2 : currentLum * currentLum, currentLum * currentLum, momentAlpha);
    result.historyLength = min(historyLength + 1.0f, maxHistoryFrames);
    result.reactive = saturate(reactive);
    result.confidence = saturate(historyConfidence);
    result.signal = float4(
        max(temporal, 0.0f.xxx),
        ResolveTemporalHitDistance(currentSignal.a, historySignal.a, validHistory, alpha));
    return result;
}

[numthreads(8, 8, 1)]
void DenoiseTemporalCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint2 dimensions = (uint2)round(g_scene.rayOptions.zw);
    uint2 pixel = dispatchThreadId.xy;
    if (pixel.x >= dimensions.x || pixel.y >= dimensions.y)
    {
        return;
    }

    float4 centerAov0 = g_denoiseAov0[pixel];
    float4 centerAov1 = g_denoiseAov1[pixel];
    float4 centerAov2 = g_denoiseAov2[pixel];
    uint centerIdentity = g_surfaceIdentity[pixel];
    TemporalHistorySample history;
    bool validSurfaceHistory = GatherTemporalHistory(
        pixel, dimensions, centerAov0, centerAov1, centerAov2, centerIdentity, history);
    float historyScore = validSurfaceHistory ? history.score : 1e20f;
    float4 previousMoments = validSurfaceHistory ? history.moments : 0.0f.xxxx;
    float4 previousLength = validSurfaceHistory ? history.lengthData : 0.0f.xxxx;
    float4 historyAov0 = validSurfaceHistory ? history.aov0 : centerAov0;
    float4 historyAov1 = validSurfaceHistory ? history.aov1 : centerAov1;
    float4 historyAov2 = validSurfaceHistory ? history.aov2 : centerAov2;

    TemporalLobeResult diffuse = ResolveTemporalLobe(
        pixel,
        dimensions,
        false,
        CurrentDiffuseSignal(pixel),
        validSurfaceHistory ? history.diffuse : 0.0f.xxxx,
        validSurfaceHistory,
        historyScore,
        previousMoments.x,
        previousMoments.y,
        previousLength.x,
        previousLength.z,
        centerAov0,
        centerAov1,
        centerAov2,
        historyAov0,
        historyAov1,
        historyAov2);
    TemporalLobeResult specular = ResolveTemporalLobe(
        pixel,
        dimensions,
        true,
        CurrentSpecularSignal(pixel),
        validSurfaceHistory ? history.specular : 0.0f.xxxx,
        validSurfaceHistory,
        historyScore,
        previousMoments.z,
        previousMoments.w,
        previousLength.y,
        previousLength.w,
        centerAov0,
        centerAov1,
        centerAov2,
        historyAov0,
        historyAov1,
        historyAov2);

    g_diffuseSpatialPing[pixel] = diffuse.signal;
    g_specularSpatialPing[pixel] = specular.signal;
    g_diffuseHistoryConfidence[pixel] = diffuse.confidence;
    g_specularHistoryConfidence[pixel] = specular.confidence;
    StoreCurrentHistories(
        pixel,
        diffuse.signal,
        specular.signal,
        float4(diffuse.mean, diffuse.mean2, specular.mean, specular.mean2),
        float4(diffuse.historyLength, specular.historyLength, diffuse.reactive, specular.reactive));
}

void RunAtrousPass(uint3 dispatchThreadId, uint passIndex)
{
    uint2 dimensions = (uint2)round(g_scene.rayOptions.zw);
    uint2 pixel = dispatchThreadId.xy;
    if (pixel.x >= dimensions.x || pixel.y >= dimensions.y || passIndex >= (uint)round(g_scene.atrousOptions.x))
    {
        return;
    }

    float4 centerAov0 = g_denoiseAov0[pixel];
    float4 centerAov1 = g_denoiseAov1[pixel];
    float4 centerAov2 = g_denoiseAov2[pixel];
    float centerDepth = centerAov0.w;
    float4 diffuseTemporal = LoadAtrousInput(pixel, passIndex, false);
    float4 specularTemporal = LoadAtrousInput(pixel, passIndex, true);
    if (centerDepth <= 0.0f || centerAov2.w <= 0.5f)
    {
        StoreAtrousOutput(pixel, passIndex, false, diffuseTemporal);
        StoreAtrousOutput(pixel, passIndex, true, specularTemporal);
        return;
    }

    float3 centerNormal = DecodeNormal(centerAov0);
    float3 centerAlbedo = max(centerAov1.rgb, 0.03f.xxx);
    float centerRoughness = saturate(centerAov1.w);
    float4 moments = LoadCurrentHistoryMoments(pixel);
    float4 historyControls = LoadCurrentHistoryLength(pixel);
    float diffuseVariance = max(moments.y - moments.x * moments.x, 0.0f);
    float specularVariance = max(moments.w - moments.z * moments.z, 0.0f);
    float normalSigma = max(g_scene.denoiseOptions.z, 0.001f);
    float depthSigma = max(g_scene.denoiseOptions.w, 0.0005f);
    float diffuseLuminanceSigma = max(g_scene.denoiseOptions2.x + sqrt(diffuseVariance) * g_scene.atrousOptions.w, 0.001f);
    float specularLuminanceSigma = max(g_scene.denoiseOptions2.x + sqrt(specularVariance) * g_scene.atrousOptions.w, 0.001f);
    float albedoSigma = max(g_scene.denoiseOptions2.y, 0.001f);
    float diffuseStrength = saturate(g_scene.denoiseOptions2.z * g_scene.atrousOptions.y * (1.0f - historyControls.z * 0.35f));
    float specularStrength = saturate(
        g_scene.denoiseOptions2.z *
        g_scene.atrousOptions.z *
        lerp(g_scene.signalDenoiseOptions.w, 1.0f, centerRoughness) *
        (1.0f - historyControls.w * 0.35f));
    int stepWidth = (int)(1u << passIndex);
    float diffuseCenterLum = Luminance(diffuseTemporal.rgb);
    float specularCenterLum = Luminance(specularTemporal.rgb);
    bool demodulateDiffuse = UseSplitSignals();
    float3 diffuseCenterValue = demodulateDiffuse ? diffuseTemporal.rgb / centerAlbedo : diffuseTemporal.rgb;
    float3 diffuseSum = diffuseCenterValue;
    float3 specularSum = specularTemporal.rgb;
    float diffuseWeightSum = 1.0f;
    float specularWeightSum = 1.0f;
    int2 centerPixel = int2(pixel);

    [unroll]
    for (int y = -2; y <= 2; ++y)
    {
        [unroll]
        for (int x = -2; x <= 2; ++x)
        {
            if (x == 0 && y == 0)
            {
                continue;
            }

            int2 samplePixel = clamp(centerPixel + int2(x, y) * stepWidth, int2(0, 0), int2((int)dimensions.x, (int)dimensions.y) - int2(1, 1));
            float4 sampleAov0 = g_denoiseAov0[uint2(samplePixel)];
            float4 sampleAov1 = g_denoiseAov1[uint2(samplePixel)];
            float4 sampleAov2 = g_denoiseAov2[uint2(samplePixel)];
            if (!ValidateAov(centerAov0, centerAov1, centerAov2, sampleAov0, sampleAov1, sampleAov2))
            {
                continue;
            }

            float4 diffuseSample = LoadAtrousInput(uint2(samplePixel), passIndex, false);
            float4 specularSample = LoadAtrousInput(uint2(samplePixel), passIndex, true);
            float3 sampleAlbedo = max(sampleAov1.rgb, 0.03f.xxx);
            float3 diffuseSampleValue = demodulateDiffuse ? diffuseSample.rgb / sampleAlbedo : diffuseSample.rgb;
            float3 sampleNormal = DecodeNormal(sampleAov0);
            float normalWeight = exp((dot(centerNormal, sampleNormal) - 1.0f) / normalSigma);
            float depthWeight = exp(-abs(sampleAov0.w - centerDepth) / max(centerDepth * depthSigma, 0.02f));
            float diffuseLuminanceWeight = exp(-abs(Luminance(diffuseSample.rgb) - diffuseCenterLum) / diffuseLuminanceSigma);
            float specularLuminanceWeight = exp(-abs(Luminance(specularSample.rgb) - specularCenterLum) / specularLuminanceSigma);
            float albedoWeight = exp(-length(sampleAlbedo - centerAlbedo) / albedoSigma);
            float spatialWeight = KernelWeight(x) * KernelWeight(y) / 256.0f;
            bool diffuseCenterHasHit = diffuseTemporal.a > 0.0f;
            bool diffuseSampleHasHit = diffuseSample.a > 0.0f;
            float diffuseHitWeight = (!diffuseCenterHasHit && !diffuseSampleHasHit)
                ? 1.0f
                : ((diffuseCenterHasHit && diffuseSampleHasHit)
                    ? exp(-abs(log2(1.0f + diffuseSample.a) - log2(1.0f + diffuseTemporal.a)) / 0.75f)
                    : 0.05f);
            bool specularCenterHasHit = specularTemporal.a > 0.0f;
            bool specularSampleHasHit = specularSample.a > 0.0f;
            float specularHitSigma = lerp(0.15f, 0.55f, centerRoughness);
            float specularHitWeight = (!specularCenterHasHit && !specularSampleHasHit)
                ? 1.0f
                : ((specularCenterHasHit && specularSampleHasHit)
                    ? exp(-abs(log2(1.0f + specularSample.a) - log2(1.0f + specularTemporal.a)) / specularHitSigma)
                    : 0.02f);
            float diffuseWeight = spatialWeight * normalWeight * depthWeight * diffuseLuminanceWeight * albedoWeight * diffuseHitWeight;
            float specularNormalWeight = pow(saturate(normalWeight), lerp(3.0f, 1.0f, centerRoughness));
            float specularWeight = spatialWeight * specularNormalWeight * depthWeight * specularLuminanceWeight * specularHitWeight;
            diffuseSum += diffuseSampleValue * diffuseWeight;
            diffuseWeightSum += diffuseWeight;
            specularSum += specularSample.rgb * specularWeight;
            specularWeightSum += specularWeight;
        }
    }

    float3 diffuseFiltered = diffuseSum / max(diffuseWeightSum, 0.0001f);
    if (demodulateDiffuse)
    {
        diffuseFiltered *= centerAlbedo;
    }
    float3 specularFiltered = specularSum / max(specularWeightSum, 0.0001f);
    // Hit distance is a geometric guide, not a radiance channel. Keep the
    // center pixel's temporally filtered distance across every spatial pass.
    StoreAtrousOutput(
        pixel,
        passIndex,
        false,
        float4(lerp(diffuseTemporal.rgb, diffuseFiltered, diffuseStrength), diffuseTemporal.a));
    StoreAtrousOutput(
        pixel,
        passIndex,
        true,
        float4(lerp(specularTemporal.rgb, specularFiltered, specularStrength), specularTemporal.a));
}

[numthreads(8, 8, 1)]
void DenoiseAtrous0CS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    RunAtrousPass(dispatchThreadId, 0u);
}

[numthreads(8, 8, 1)]
void DenoiseAtrous1CS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    RunAtrousPass(dispatchThreadId, 1u);
}

[numthreads(8, 8, 1)]
void DenoiseAtrous2CS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    RunAtrousPass(dispatchThreadId, 2u);
}

[numthreads(8, 8, 1)]
void DenoiseAtrous3CS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    RunAtrousPass(dispatchThreadId, 3u);
}

[numthreads(8, 8, 1)]
void DenoiseAtrous4CS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    RunAtrousPass(dispatchThreadId, 4u);
}

[numthreads(8, 8, 1)]
void DenoiseCompositeCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint2 dimensions = (uint2)round(g_scene.rayOptions.zw);
    uint2 pixel = dispatchThreadId.xy;
    if (pixel.x >= dimensions.x || pixel.y >= dimensions.y)
    {
        return;
    }

    uint debugMode = (uint)round(g_scene.debugOptions.x);
    float3 currentColor = CurrentSignal(pixel);
    float4 temporalDiffuse = LoadCurrentDiffuseHistory(pixel);
    float4 temporalSpecular = LoadCurrentSpecularHistory(pixel);
    float3 residualSignal = CurrentResidualSignal(pixel);
    float3 temporalColor = temporalDiffuse.rgb + temporalSpecular.rgb + residualSignal;
    float4 filteredDiffuse = LoadFinalAtrous(pixel, false);
    float4 filteredSpecular = LoadFinalAtrous(pixel, true);
    float3 filtered = filteredDiffuse.rgb + filteredSpecular.rgb + residualSignal;
    float3 directSignal = max(g_signalDirect[pixel].rgb, 0.0f.xxx);
    float3 indirectSignal = max(g_signalIndirect[pixel].rgb, 0.0f.xxx);
    float4 centerAov2 = g_denoiseAov2[pixel];
    float4 historyLength = LoadCurrentHistoryLength(pixel);
    float4 moments = LoadCurrentHistoryMoments(pixel);
    float diffuseVariance = max(moments.y - moments.x * moments.x, 0.0f);
    float specularVariance = max(moments.w - moments.z * moments.z, 0.0f);
    float variance = max(diffuseVariance, specularVariance);
    uint pixelIndex = RtxdiReservoirPointer(pixel, dimensions);
    RestirReservoir reservoir = (RestirReservoir)0;
    // RTXDI-disabled builds bind a one-element placeholder reservoir. Never
    // index it per-pixel; reservoir debug views stay black during Baseline PT.
    if (g_scene.pathOptions.w > 0.5f)
    {
        reservoir = g_restirHistory[pixelIndex];
    }
    float maxHistory = max(g_scene.reconstructionOptions.y, 1.0f);
    float maxReservoirAge = max(g_scene.restirStabilityOptions.w, 1.0f);

    if (debugMode == 16u) filtered = currentColor;
    if (debugMode == 17u) filtered = temporalColor;
    if (debugMode == 18u) filtered = saturate(min(historyLength.x, historyLength.y) / maxHistory).xxx;
    if (debugMode == 19u) filtered = saturate(variance / max(variance + 1.0f, 0.0001f)).xxx;
    if (debugMode == 20u) filtered = float3(0.5f + centerAov2.x * 20.0f, 0.5f + centerAov2.y * 20.0f, 0.5f);
    if (debugMode == 21u) filtered = (min(historyLength.x, historyLength.y) <= 1.0f).xxx;
    if (debugMode == 22u) filtered = indirectSignal;
    if (debugMode == 23u) filtered = abs(temporalColor - filtered) * 4.0f;
    uint reservoirAge = (reservoir.distanceAge >> 16u) & 0xffu;
    bool reservoirValid = (reservoir.lightData & 0x80000000u) != 0u && reservoir.targetPdf > 0.0f;
    if (debugMode == 24u) filtered = saturate((float)reservoirAge / maxReservoirAge).xxx;
    if (debugMode == 25u) filtered = (reservoirValid ? 1.0f : 0.0f).xxx;
    if (debugMode == 32u) filtered = directSignal;
    if (debugMode == 33u) filtered = indirectSignal;
    if (debugMode == 34u) filtered = residualSignal;
    if (debugMode == 35u) filtered = currentColor;
    if (debugMode == 36u) filtered = temporalColor;
    if (debugMode == 37u) filtered = filteredDiffuse.rgb + filteredSpecular.rgb + residualSignal;
    if (debugMode == 38u) filtered = max(historyLength.z, historyLength.w).xxx;
    if (debugMode == 39u) filtered = min(g_diffuseHistoryConfidence[pixel], g_specularHistoryConfidence[pixel]).xxx;
    if (debugMode == 40u) filtered = saturate(specularVariance / max(specularVariance + 1.0f, 0.0001f)).xxx;
    if (debugMode == 41u) filtered = DecodeNormal(g_denoiseAov0[pixel]) * 0.5f + 0.5f;
    if (debugMode == 42u) filtered = saturate(g_denoiseAov1[pixel].w).xxx;
    if (debugMode == 43u) filtered = VisualizeLinearViewZ(abs(g_denoiseAov0[pixel].w));
    if (debugMode == 44u) filtered = VisualizeMotion25D(g_denoiseAov2[pixel].xyz, g_denoiseAov0[pixel].w);
    if (debugMode == 45u)
    {
        bool surface = g_denoiseAov2[pixel].w > 0.0f;
        bool validHistory = surface && min(historyLength.x, historyLength.y) > 1.0f;
        // Red = rejected/disoccluded surface, green = accepted reprojected
        // surface history, blue = background/no primary surface.
        filtered = surface
            ? (validHistory ? float3(0.0f, 1.0f, 0.0f) : float3(1.0f, 0.0f, 0.0f))
            : float3(0.0f, 0.0f, 1.0f);
    }
    if (debugMode == 46u)
    {
        float4 aov0 = g_denoiseAov0[pixel];
        float4 aov1 = g_denoiseAov1[pixel];
        float4 aov2 = g_denoiseAov2[pixel];
        bool nonFinite =
            Invalid4(aov0) || Invalid4(aov1) || Invalid4(aov2) ||
            Invalid4(g_signalDirect[pixel]) ||
            Invalid4(g_signalIndirect[pixel]) ||
            Invalid4(g_signalResidual[pixel]) ||
            Invalid4(LoadCurrentDiffuseHistory(pixel)) ||
            Invalid4(LoadCurrentSpecularHistory(pixel)) ||
            Invalid4(LoadCurrentHistoryMoments(pixel)) ||
            Invalid4(historyLength);
        float encodedNormalLength = length(aov0.xyz * 2.0f - 1.0f);
        bool semanticallyInvalid = aov2.w > 0.0f &&
            (aov0.w <= 0.0f || encodedNormalLength < 0.95f || encodedNormalLength > 1.05f ||
                aov1.w < 0.0f || aov1.w > 1.0f);
        // Green = finite and contract-valid, yellow = finite but outside the
        // guide contract, magenta = NaN or Inf in any input/history signal.
        filtered = nonFinite
            ? float3(1.0f, 0.0f, 1.0f)
            : (semanticallyInvalid ? float3(1.0f, 1.0f, 0.0f) : float3(0.0f, 1.0f, 0.0f));
    }

    g_postDenoiseHdr[pixel] = float4(max(filtered, 0.0f.xxx), 1.0f);
    if (g_scene.denoisePassOptions.z < 0.5f)
    {
        g_output[pixel] = float4(Tonemap(filtered), 1.0f);
    }
}
