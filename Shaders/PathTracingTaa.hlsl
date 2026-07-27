#if defined(VULKAN)
#define VK_BINDING(slot, descriptorSet) [[vk::binding(slot, descriptorSet)]]
#else
#define VK_BINDING(binding, set)
#endif

#ifndef D3D12LOOKDEVPT_WITH_NRD
#define D3D12LOOKDEVPT_WITH_NRD 1
#endif

#if D3D12LOOKDEVPT_WITH_NRD
#include "../ThirdParty/NRD/Shaders/NRD.hlsli"
#else
// FinalTaaCS remains shader-compile compatible when the optional NRD SDK is
// disabled. Runtime never enables fusion in that build, but DXC still needs
// the reconstruction helpers referenced by the statically compiled branch.
float4 REBLUR_BackEnd_UnpackRadianceAndNormHitDist(float4 packed)
{
    return packed;
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

VK_BINDING(1, 0) RWTexture2D<float4> g_output : register(u0, space0);
VK_BINDING(3, 0) ConstantBuffer<SceneConstants> g_scene : register(b0, space0);
VK_BINDING(13, 0) RWTexture2D<float4> g_denoiseAov0 : register(u5, space0);
VK_BINDING(14, 0) RWTexture2D<float4> g_denoiseAov1 : register(u6, space0);
VK_BINDING(15, 0) RWTexture2D<float4> g_denoiseAov2 : register(u7, space0);
VK_BINDING(19, 0) RWTexture2D<float4> g_previousDenoiseAov0 : register(u11, space0);
VK_BINDING(20, 0) RWTexture2D<float4> g_previousDenoiseAov1 : register(u12, space0);
VK_BINDING(21, 0) RWTexture2D<float4> g_previousDenoiseAov2 : register(u13, space0);
VK_BINDING(26, 0) RWTexture2D<float4> g_signalDirect : register(u18, space0);
VK_BINDING(27, 0) RWTexture2D<float4> g_signalIndirect : register(u19, space0);
VK_BINDING(28, 0) RWTexture2D<float4> g_signalResidual : register(u20, space0);
VK_BINDING(36, 0) RWTexture2D<float4> g_nrdDiffDenoised : register(u28, space0);
VK_BINDING(37, 0) RWTexture2D<float4> g_nrdSpecDenoised : register(u29, space0);
VK_BINDING(41, 0) RWTexture2D<float4> g_postDenoiseHdr : register(u33, space0);
VK_BINDING(42, 0) RWTexture2D<float4> g_taaHistoryA : register(u34, space0);
VK_BINDING(43, 0) RWTexture2D<float4> g_taaHistoryB : register(u35, space0);
VK_BINDING(46, 0) RWTexture2D<uint> g_surfaceIdentity : register(u38, space0);
VK_BINDING(47, 0) RWTexture2D<uint> g_previousSurfaceIdentity : register(u39, space0);
VK_BINDING(48, 0) RWTexture2D<float4> g_finalResolvedHdr : register(u40, space0);
#include "PathTracingQualityCounters.hlsli"

static const float TaaEpsilon = 1e-5f;
groupshared float3 g_currentHdrTile[100];

float Luminance(float3 color)
{
    return dot(color, float3(0.2126f, 0.7152f, 0.0722f));
}

float3 LinearToYCoCg(float3 color)
{
    return float3(
        dot(color, float3(0.25f, 0.5f, 0.25f)),
        dot(color, float3(0.5f, 0.0f, -0.5f)),
        dot(color, float3(-0.25f, 0.5f, -0.25f)));
}

float3 YCoCgToLinear(float3 color)
{
    float t = color.x - color.z;
    return max(float3(t + color.y, color.x + color.z, t - color.y), 0.0f.xxx);
}

float3 DecodeNormal(float4 aov)
{
    float3 normal = aov.xyz * 2.0f - 1.0f;
    return dot(normal, normal) > TaaEpsilon ? normalize(normal) : float3(0.0f, 1.0f, 0.0f);
}

float3 ViewDirectionFromPixel(uint2 pixel, uint2 dimensions)
{
    float2 uv = (float2(pixel) + 0.5f.xx + g_scene.jitterOptions.xy) / float2(dimensions);
    float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    float4 nearPoint = mul(float4(ndc, 0.0f, 1.0f), g_scene.inverseViewProjection);
    float4 farPoint = mul(float4(ndc, 1.0f, 1.0f), g_scene.inverseViewProjection);
    nearPoint.xyz /= max(abs(nearPoint.w), TaaEpsilon);
    farPoint.xyz /= max(abs(farPoint.w), TaaEpsilon);
    return normalize(nearPoint.xyz - farPoint.xyz);
}

void MaterialFactors(
    uint2 pixel,
    uint2 dimensions,
    float3 normal,
    float roughness,
    float3 albedo,
    float metallic,
    out float3 diffuseFactor,
    out float3 specularFactor)
{
    float3 reflectance0 = lerp(0.04f.xxx, albedo, metallic);
    NRD_MaterialFactors(
        normal,
        ViewDirectionFromPixel(pixel, dimensions),
        albedo * (1.0f - metallic),
        reflectance0,
        roughness,
        diffuseFactor,
        specularFactor);
}

float3 CurrentSignal(uint2 pixel)
{
    return max(g_signalDirect[pixel].rgb, 0.0f.xxx) +
        max(g_signalIndirect[pixel].rgb, 0.0f.xxx) +
        max(g_signalResidual[pixel].rgb, 0.0f.xxx);
}

float3 QuantizePostDenoiseHdr(float3 color)
{
    // The removed intermediate is R16G16B16A16_FLOAT. Preserve its rounding
    // contract so fusion changes bandwidth and dispatch count, not TAA history.
    return f16tof32(f32tof16(max(color, 0.0f.xxx)));
}

float3 LoadNrdReconstructedHdr(uint2 pixel, uint2 dimensions)
{
    float3 currentColor = CurrentSignal(pixel);
    bool hit = g_denoiseAov2[pixel].w > 0.0f;
    if (!hit)
    {
        return QuantizePostDenoiseHdr(currentColor);
    }

    bool relax = g_scene.denoisePassOptions.x > 0.5f;
    float4 diffData = g_nrdDiffDenoised[pixel];
    float4 specData = g_nrdSpecDenoised[pixel];
    float3 diffuse = relax
        ? max(diffData.rgb, 0.0f.xxx)
        : max(REBLUR_BackEnd_UnpackRadianceAndNormHitDist(diffData).rgb, 0.0f.xxx);
    float3 specular = relax
        ? max(specData.rgb, 0.0f.xxx)
        : max(REBLUR_BackEnd_UnpackRadianceAndNormHitDist(specData).rgb, 0.0f.xxx);
    float4 aov0 = g_denoiseAov0[pixel];
    float4 aov1 = g_denoiseAov1[pixel];
    float3 diffuseFactor;
    float3 specularFactor;
    MaterialFactors(
        pixel,
        dimensions,
        DecodeNormal(aov0),
        saturate(aov1.w),
        max(aov1.rgb, 0.0f.xxx),
        saturate(g_signalResidual[pixel].a),
        diffuseFactor,
        specularFactor);
    float3 filtered = max(
        diffuse * diffuseFactor +
        specular * specularFactor +
        max(g_signalResidual[pixel].rgb, 0.0f.xxx),
        0.0f.xxx);
    filtered = (any(isnan(filtered)) || any(isinf(filtered))) ? currentColor : filtered;
    return QuantizePostDenoiseHdr(filtered);
}

float3 LoadCurrentHdr(uint2 pixel, uint2 dimensions)
{
    if (g_scene.postProcessOptions.x > 0.5f && (uint)round(g_scene.debugOptions.x) == 0u)
    {
        return LoadNrdReconstructedHdr(pixel, dimensions);
    }
    return max(g_postDenoiseHdr[pixel].rgb, 0.0f.xxx);
}

bool IsInside(int2 pixel, uint2 dimensions)
{
    return pixel.x >= 0 && pixel.y >= 0 && pixel.x < (int)dimensions.x && pixel.y < (int)dimensions.y;
}

bool PreviousHistoryIsA()
{
    return (g_scene.frameOptions.w & 1u) == 0u;
}

float4 LoadPreviousHistory(uint2 pixel)
{
    return PreviousHistoryIsA() ? g_taaHistoryA[pixel] : g_taaHistoryB[pixel];
}

void StoreCurrentHistory(uint2 pixel, float4 value)
{
    if (PreviousHistoryIsA())
    {
        g_taaHistoryB[pixel] = value;
    }
    else
    {
        g_taaHistoryA[pixel] = value;
    }
}

bool ValidateSurface(
    float4 currentAov0,
    float4 currentAov1,
    float4 currentAov2,
    uint currentIdentity,
    float4 previousAov0,
    float4 previousAov1,
    float4 previousAov2,
    uint previousIdentity)
{
    const bool currentHit = currentAov2.w > 0.0f;
    const bool previousHit = previousAov2.w > 0.0f;
    if (currentHit != previousHit)
    {
        // With a stationary camera, a hit/miss toggle at a silhouette or an
        // alpha-tested edge is temporal coverage, not a disocclusion. Accept
        // the tap so the 32-phase jitter sequence can integrate that coverage.
        // During camera motion the same toggle is a real reveal and must reject
        // history to avoid a foreground trail over newly exposed background.
        return g_scene.stabilityOptions.y <= 0.001f;
    }
    if (!currentHit)
    {
        return true;
    }
    if (!ValidatePackedSurfaceGroup(currentIdentity, previousIdentity))
    {
        return false;
    }
    bool samePrimitive = ValidatePackedPrimitiveIdentity(currentIdentity, previousIdentity);

    float normalDot = dot(DecodeNormal(currentAov0), DecodeNormal(previousAov0));
    // Motion.z is previousViewZ - currentViewZ. Validate against the depth
    // expected in the previous camera, otherwise ordinary dolly/orbit motion
    // rejects otherwise correct surface history.
    float expectedPreviousViewZ = max(currentAov0.w + currentAov2.z, TaaEpsilon);
    float depthDelta = abs(expectedPreviousViewZ - previousAov0.w) / max(expectedPreviousViewZ, 1.0f);
    float roughnessDelta = abs(currentAov1.w - previousAov1.w);
    // Textured albedo is a shading signal, not a stable geometric guide. The
    // same surface can legitimately sample a different texture footprint as
    // jitter and ray cones evolve. Material/instance/primitive identity already
    // protects true material discontinuities, so rejecting on RGB here caused
    // persistent false disocclusions on otherwise static textured surfaces.
    bool guidesMatch = normalDot >= g_scene.validationOptions.x &&
        depthDelta <= g_scene.validationOptions.y &&
        roughnessDelta <= g_scene.validationOptions.w;
    // Fine tessellation should not turn a sub-pixel camera jitter into a
    // disocclusion. Across a primitive boundary, require a much tighter
    // geometric continuation while retaining the same surface group.
    bool continuousTriangleSeam = !samePrimitive &&
        normalDot >= max(g_scene.validationOptions.x, 0.985f) &&
        depthDelta <= min(g_scene.validationOptions.y, 0.0025f) &&
        roughnessDelta <= min(g_scene.validationOptions.w, 0.025f);
    return guidesMatch && (samePrimitive || continuousTriangleSeam);
}

float2 ProjectWorldToUv(float3 worldPosition, float4x4 viewProjection)
{
    float4 clip = mul(float4(worldPosition, 1.0f), viewProjection);
    float2 ndc = clip.xy / max(abs(clip.w), TaaEpsilon);
    return float2(ndc.x * 0.5f + 0.5f, 0.5f - ndc.y * 0.5f);
}

float2 BackgroundMotion(uint2 pixel, uint2 dimensions)
{
    float2 uv = (float2(pixel) + 0.5f.xx) / float2(dimensions);
    float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    float4 nearPoint = mul(float4(ndc, 0.0f, 1.0f), g_scene.inverseViewProjection);
    float4 farPoint = mul(float4(ndc, 1.0f, 1.0f), g_scene.inverseViewProjection);
    nearPoint.xyz /= max(abs(nearPoint.w), TaaEpsilon);
    farPoint.xyz /= max(abs(farPoint.w), TaaEpsilon);
    float3 direction = normalize(farPoint.xyz - nearPoint.xyz);
    float3 distantPoint = g_scene.cameraPosition.xyz + direction * 100000.0f;
    return ProjectWorldToUv(distantPoint, g_scene.previousViewProjection) - uv;
}

bool GatherResolvedHistory(
    float2 historyPosition,
    uint2 dimensions,
    float2 jitterDelta,
    float4 currentAov0,
    float4 currentAov1,
    float4 currentAov2,
    uint currentIdentity,
    out float3 historyColor,
    out float historyLength)
{
    historyColor = 0.0f.xxx;
    historyLength = 0.0f;
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
            float weight = (x == 0 ? 1.0f - fraction.x : fraction.x) *
                (y == 0 ? 1.0f - fraction.y : fraction.y);
            if (weight <= TaaEpsilon)
            {
                continue;
            }
            // Color history lives on the output grid, while the matching
            // previous SurfaceGuide is a raw jittered sample. Validate every
            // bilinear color tap at its corresponding guide location so a
            // moving silhouette cannot mix foreground and background history.
            int2 guideCandidate = int2(round(float2(candidate) + jitterDelta));
            if (!IsInside(guideCandidate, dimensions))
            {
                continue;
            }
            uint2 guidePixel = uint2(guideCandidate);
            if (!ValidateSurface(
                currentAov0,
                currentAov1,
                currentAov2,
                currentIdentity,
                g_previousDenoiseAov0[guidePixel],
                g_previousDenoiseAov1[guidePixel],
                g_previousDenoiseAov2[guidePixel],
                g_previousSurfaceIdentity[guidePixel]))
            {
                continue;
            }
            float4 sample = LoadPreviousHistory(uint2(candidate));
            if (sample.a <= 0.0f || any(isnan(sample)) || any(isinf(sample)))
            {
                continue;
            }
            historyColor += max(sample.rgb, 0.0f.xxx) * weight;
            historyLength += sample.a * weight;
            totalWeight += weight;
        }
    }

    if (totalWeight <= TaaEpsilon)
    {
        historyColor = 0.0f.xxx;
        historyLength = 0.0f;
        return false;
    }
    historyColor /= totalWeight;
    historyLength /= totalWeight;
    return true;
}

bool GatherStationaryCoverageHistory(
    uint2 pixel,
    out float3 historyColor,
    out float historyLength)
{
    historyColor = 0.0f.xxx;
    historyLength = 0.0f;
    if (g_scene.stabilityOptions.y > 0.001f)
    {
        return false;
    }
    float4 stationaryHistory = LoadPreviousHistory(pixel);
    if (stationaryHistory.a <= 0.0f ||
        any(isnan(stationaryHistory)) || any(isinf(stationaryHistory)))
    {
        return false;
    }
    historyColor = max(stationaryHistory.rgb, 0.0f.xxx);
    historyLength = stationaryHistory.a;
    return true;
}

bool GatherHistory(uint2 pixel, uint2 dimensions, float2 motion, out float3 historyColor, out float historyLength)
{
    historyColor = 0.0f.xxx;
    historyLength = 0.0f;
    uint historyDomains = (uint)round(g_scene.environmentOptions.w);
    if ((historyDomains & (HISTORY_DOMAIN_SURFACE | HISTORY_DOMAIN_TAA)) !=
        (HISTORY_DOMAIN_SURFACE | HISTORY_DOMAIN_TAA))
    {
        return false;
    }

    float2 jitterDelta = g_scene.jitterOptions.xy - g_scene.jitterOptions.zw;
    // Previous SurfaceGuides are raw jittered samples, so their validation
    // position includes the current-to-previous jitter delta. TAA color history
    // is already resolved onto the non-jittered output grid and must not be
    // shifted again; doing so causes a stationary image to random-walk and blur.
    float2 guideHistoryPosition = float2(pixel) + motion * float2(dimensions) + jitterDelta;
    float2 resolvedHistoryPosition = float2(pixel) + motion * float2(dimensions);
    int2 basePixel = int2(floor(guideHistoryPosition));
    float2 fraction = frac(guideHistoryPosition);
    float totalWeight = 0.0f;
    float4 currentAov0 = g_denoiseAov0[pixel];
    float4 currentAov1 = g_denoiseAov1[pixel];
    float4 currentAov2 = g_denoiseAov2[pixel];
    uint currentIdentity = g_surfaceIdentity[pixel];

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
            if (!ValidateSurface(
                currentAov0,
                currentAov1,
                currentAov2,
                currentIdentity,
                g_previousDenoiseAov0[candidatePixel],
                g_previousDenoiseAov1[candidatePixel],
                g_previousDenoiseAov2[candidatePixel],
                g_previousSurfaceIdentity[candidatePixel]))
            {
                continue;
            }

            float weight = (x == 0 ? 1.0f - fraction.x : fraction.x) *
                (y == 0 ? 1.0f - fraction.y : fraction.y);
            totalWeight += weight;
        }
    }

    if (totalWeight <= TaaEpsilon)
    {
        // A newly revealed edge can invalidate all four bilinear taps even
        // though a compatible history sample exists immediately beside the
        // reprojected footprint. Use a small depth/normal-aware dilation only
        // in that case; never mix it into a valid bilinear reconstruction.
        float bestScore = 1e20f;
        int2 centerPixel = int2(round(guideHistoryPosition));
        [unroll]
        for (int dilationY = -1; dilationY <= 1; ++dilationY)
        {
            [unroll]
            for (int dilationX = -1; dilationX <= 1; ++dilationX)
            {
                int2 candidate = centerPixel + int2(dilationX, dilationY);
                if (!IsInside(candidate, dimensions))
                {
                    continue;
                }
                uint2 candidatePixel = uint2(candidate);
                float4 previousAov0 = g_previousDenoiseAov0[candidatePixel];
                float4 previousAov1 = g_previousDenoiseAov1[candidatePixel];
                float4 previousAov2 = g_previousDenoiseAov2[candidatePixel];
                if (!ValidateSurface(
                    currentAov0,
                    currentAov1,
                    currentAov2,
                    currentIdentity,
                    previousAov0,
                    previousAov1,
                    previousAov2,
                    g_previousSurfaceIdentity[candidatePixel]))
                {
                    continue;
                }

                float depthScore = currentAov2.w > 0.0f
                    ? abs(currentAov0.w - previousAov0.w) / max(currentAov0.w, 1.0f)
                    : 0.0f;
                float normalScore = currentAov2.w > 0.0f
                    ? 1.0f - saturate(dot(DecodeNormal(currentAov0), DecodeNormal(previousAov0)))
                    : 0.0f;
                float spatialScore = dot(float2(dilationX, dilationY), float2(dilationX, dilationY)) * 0.01f;
                float score = depthScore + normalScore + spatialScore;
                if (score < bestScore)
                {
                    bestScore = score;
                }
            }
        }
        if (bestScore >= 1e19f)
        {
            // With a stationary camera and an unchanged TAA domain, failure of
            // every guide tap is usually sub-pixel coverage switching between
            // two surfaces as the jitter phase advances. Reuse the same-pixel
            // resolve as a deliberately biased coverage lock. Camera cuts,
            // geometry/material/light edits, and ordinary camera motion cannot
            // enter this path because the host invalidates the domain or the
            // motion gate below fails.
            return GatherStationaryCoverageHistory(pixel, historyColor, historyLength);
        }
        bool resolvedValid = GatherResolvedHistory(
            resolvedHistoryPosition,
            dimensions,
            jitterDelta,
            currentAov0,
            currentAov1,
            currentAov2,
            currentIdentity,
            historyColor,
            historyLength);
        if (resolvedValid)
        {
            return true;
        }
        return GatherStationaryCoverageHistory(pixel, historyColor, historyLength);
    }
    bool resolvedValid = GatherResolvedHistory(
        resolvedHistoryPosition,
        dimensions,
        jitterDelta,
        currentAov0,
        currentAov1,
        currentAov2,
        currentIdentity,
        historyColor,
        historyLength);
    if (resolvedValid)
    {
        return true;
    }
    return GatherStationaryCoverageHistory(pixel, historyColor, historyLength);
}

void NeighborhoodBounds(uint2 groupThreadId, out float3 lowValue, out float3 highValue, out float3 neighborAverage)
{
    lowValue = float3(1e20f, 1e20f, 1e20f);
    highValue = float3(-1e20f, -1e20f, -1e20f);
    neighborAverage = 0.0f.xxx;
    float weight = 0.0f;
    [unroll]
    for (int y = -1; y <= 1; ++y)
    {
        [unroll]
        for (int x = -1; x <= 1; ++x)
        {
            uint tileX = groupThreadId.x + 1u + x;
            uint tileY = groupThreadId.y + 1u + y;
            float3 sample = g_currentHdrTile[tileY * 10u + tileX];
            float3 ycocg = LinearToYCoCg(sample);
            lowValue = min(lowValue, ycocg);
            highValue = max(highValue, ycocg);
            if (abs(x) + abs(y) == 1)
            {
                neighborAverage += sample;
                weight += 1.0f;
            }
        }
    }
    neighborAverage /= max(weight, 1.0f);
}

float3 PreviousHistoryNeighborAverage(uint2 pixel, uint2 dimensions)
{
    float3 average = 0.0f.xxx;
    float weight = 0.0f;
    int2 center = int2(pixel);
    const int2 offsets[4] =
    {
        int2(-1, 0), int2(1, 0), int2(0, -1), int2(0, 1)
    };
    [unroll]
    for (uint index = 0u; index < 4u; ++index)
    {
        int2 candidate = center + offsets[index];
        if (!IsInside(candidate, dimensions))
        {
            continue;
        }
        float4 sample = LoadPreviousHistory(uint2(candidate));
        if (sample.a <= 0.0f)
        {
            continue;
        }
        // Do not sharpen across a silhouette, material boundary, or unrelated
        // primitive. Those boundaries are where an unguarded unsharp mask
        // creates the most visible halo and temporal crawling.
        if (!ValidateSurface(
            g_denoiseAov0[pixel],
            g_denoiseAov1[pixel],
            g_denoiseAov2[pixel],
            g_surfaceIdentity[pixel],
            g_previousDenoiseAov0[uint2(candidate)],
            g_previousDenoiseAov1[uint2(candidate)],
            g_previousDenoiseAov2[uint2(candidate)],
            g_previousSurfaceIdentity[uint2(candidate)]))
        {
            continue;
        }
        average += max(sample.rgb, 0.0f.xxx);
        weight += 1.0f;
    }
    return weight > 0.0f ? average / weight : max(LoadPreviousHistory(pixel).rgb, 0.0f.xxx);
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
        color /= 1.0f.xxx + color;
    }
    else if (toneMapper == 2u)
    {
        color = AcesTonemap(color);
    }
    return pow(saturate(color), 1.0f / max(g_scene.viewOptions.y, 0.01f));
}

[numthreads(8, 8, 1)]
void FinalTaaCS(
    uint3 dispatchThreadId : SV_DispatchThreadID,
    uint3 groupThreadId : SV_GroupThreadID,
    uint3 groupId : SV_GroupID)
{
    uint2 dimensions = (uint2)round(g_scene.rayOptions.zw);
    uint2 pixel = dispatchThreadId.xy;

    // Reconstruct each current HDR sample once per 8x8 group, including a
    // one-pixel halo for the existing 3x3 clamp. This keeps fusion from
    // repeating NRD material demodulation nine times per output pixel.
    uint groupLinearIndex = groupThreadId.y * 8u + groupThreadId.x;
    [loop]
    for (uint tileIndex = groupLinearIndex; tileIndex < 100u; tileIndex += 64u)
    {
        uint tileX = tileIndex % 10u;
        uint tileY = tileIndex / 10u;
        int2 sourcePixel = int2(groupId.xy * 8u) + int2(tileX, tileY) - 1;
        sourcePixel = clamp(sourcePixel, int2(0, 0), int2(dimensions) - 1);
        g_currentHdrTile[tileIndex] = LoadCurrentHdr(uint2(sourcePixel), dimensions);
    }
    GroupMemoryBarrierWithGroupSync();

    if (pixel.x >= dimensions.x || pixel.y >= dimensions.y)
    {
        return;
    }

    float3 current = g_currentHdrTile[(groupThreadId.y + 1u) * 10u + groupThreadId.x + 1u];
    if (any(isnan(current)) || any(isinf(current)))
    {
        current = 0.0f.xxx;
    }

    // Contract-validation views are categorical data. Temporal blending,
    // neighborhood clipping, exposure, and sharpening would hide single-pixel
    // failures, so present them 1:1 while leaving the underlying guide-history
    // update path independent of final TAA.
    uint debugMode = (uint)round(g_scene.debugOptions.x);
    if (debugMode >= 41u && debugMode <= 46u)
    {
        g_finalResolvedHdr[pixel] = float4(saturate(current), 1.0f);
        g_output[pixel] = float4(saturate(current), 1.0f);
        return;
    }

    bool hit = g_denoiseAov2[pixel].w > 0.0f;
    float2 motion = hit ? g_denoiseAov2[pixel].xy : BackgroundMotion(pixel, dimensions);
    float3 historyColor;
    float historyLength;
    bool validHistory = GatherHistory(pixel, dimensions, motion, historyColor, historyLength);
    StoreQualityTaaDecision(pixel, dimensions, true, validHistory);

    // Scene/material/light edits invalidate the TAA domain on the host. Within
    // a valid domain, motion is the anti-lag signal; ordinary Monte-Carlo
    // variance must not be treated as a reactive event.
    float motionReactive = saturate(length(motion) * 48.0f);
    float reactive = motionReactive;
    float historyLock = validHistory
        ? saturate(historyLength / 16.0f) * (1.0f - reactive)
        : 0.0f;

    float3 lowValue;
    float3 highValue;
    float3 neighborAverage;
    NeighborhoodBounds(groupThreadId.xy, lowValue, highValue, neighborAverage);
    float3 historyYCoCg = LinearToYCoCg(historyColor);
    float3 extent = max(highValue - lowValue, float3(0.02f, 0.01f, 0.01f));
    float3 clippedHistoryYCoCg = clamp(historyYCoCg, lowValue - extent * 0.15f, highValue + extent * 0.15f);
    // A stochastic current neighborhood is useful while history is young or
    // moving, but clamping a locked history to it re-injects temporal noise even
    // when the explicit blend weight is tiny.
    float historyClipStrength = saturate(max(1.0f - historyLock, reactive));
    historyColor = YCoCgToLinear(lerp(historyYCoCg, clippedHistoryYCoCg, historyClipStrength));

    // Newly stopped motion converges aggressively for the first few frames;
    // a locked static surface then uses a long effective integration window.
    float lockedMinimumWeight = lerp(0.50f, 0.0005f, historyLock);
    float currentWeight = validHistory
        ? max(1.0f / (historyLength + 1.0f), lockedMinimumWeight)
        : 1.0f;
    // Once a surface is locked, the game-quality profile deliberately uses a
    // biased long temporal window. Apply that floor explicitly instead of
    // letting the nominal history length choose the final static blend.
    currentWeight = lerp(currentWeight, min(currentWeight, 0.0005f), historyLock * historyLock);
    // Preserve stop-and-go responsiveness: for the configured settle interval,
    // rapidly replace motion-biased history, then ease into the long static
    // window. The first four 8-frame-default settle samples remove over 90% of
    // a stale contribution without an abrupt quality pop.
    float settleProgress = saturate(g_scene.denoiseOptions2.w);
    float stopRecoveryWeight = lerp(
        0.65f,
        0.0005f,
        smoothstep(0.125f, 1.0f, settleProgress));
    currentWeight = validHistory ? max(currentWeight, stopRecoveryWeight) : currentWeight;
    // REBLUR itself refines for roughly 30 frames. Locking TAA at frame eight
    // freezes its early, wider result and makes a stopped image look lower
    // resolution. Keep a 32-frame aperture until the denoiser is mature, then
    // transition to the long stability window.
    float denoiserMaturity = saturate(g_scene.stabilityOptions.w);
    float denoiserRefinementWeight = lerp(
        1.0f / 32.0f,
        0.0005f,
        smoothstep(0.75f, 1.0f, denoiserMaturity));
    currentWeight = validHistory ? max(currentWeight, denoiserRefinementWeight) : currentWeight;
    currentWeight = lerp(currentWeight, max(currentWeight, 0.55f), reactive);
    float3 resolved = lerp(historyColor, current, saturate(currentWeight));
    float nextHistoryLength = validHistory
        ? min(historyLength + 1.0f, 512.0f)
        : 1.0f;
    StoreCurrentHistory(pixel, float4(resolved, nextHistoryLength));

    // Sharpen only a genuinely static locked history and derive its neighborhood
    // from the immutable previous resolve. Using the current denoiser output here
    // leaks jitter/noise back into an otherwise stable result.
    float2 motionPixels = motion * float2(dimensions);
    float staticSharpen = 1.0f - saturate(length(motionPixels) * 4.0f);
    float3 stableNeighborAverage = PreviousHistoryNeighborAverage(pixel, dimensions);
    float resolvedLuma = Luminance(resolved);
    float neighborLuma = Luminance(stableNeighborAverage);
    float relativeContrast = abs(resolvedLuma - neighborLuma) /
        max(max(resolvedLuma, neighborLuma), 0.05f);
    // The configured 0.15 is the maximum strength. Delay it until the temporal
    // estimate is mature, avoid amplifying flat-field Monte-Carlo noise, and
    // suppress extreme edges where sharpening would make halos or alpha crawl.
    float matureHistory = historyLock * smoothstep(0.5f, 1.0f, settleProgress);
    float visibleSignal = smoothstep(0.02f, 0.10f, resolvedLuma);
    float usefulContrast = smoothstep(0.015f, 0.08f, relativeContrast) *
        (1.0f - smoothstep(0.65f, 1.25f, relativeContrast));
    float sharpenStrength = saturate(g_scene.denoisePassOptions.w) *
        staticSharpen * matureHistory * visibleSignal * usefulContrast;
    float3 sharpened = resolved + (resolved - stableNeighborAverage) * sharpenStrength;
    float3 clippedSharpenedYCoCg = clamp(LinearToYCoCg(sharpened), lowValue - extent * 0.1f, highValue + extent * 0.1f);
    // Current-frame bounds remain a safety net for young/reactive history only.
    float finalClipStrength = saturate(1.0f - historyLock);
    float3 finalHdr = YCoCgToLinear(lerp(LinearToYCoCg(sharpened), clippedSharpenedYCoCg, finalClipStrength));
    if (debugMode == 47u)
    {
        finalHdr = (saturate(historyLength / 512.0f)).xxx;
    }
    else if (debugMode == 48u)
    {
        finalHdr = (validHistory ? 1.0f : 0.0f).xxx;
    }
    g_finalResolvedHdr[pixel] = float4(finalHdr, 1.0f);
    g_output[pixel] = float4(Tonemap(finalHdr), 1.0f);
}
