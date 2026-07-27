#if defined(VULKAN)
#define VK_BINDING(slot, descriptorSet) [[vk::binding(slot, descriptorSet)]]
#else
#define VK_BINDING(binding, set)
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
VK_BINDING(41, 0) RWTexture2D<float4> g_postDenoiseHdr : register(u33, space0);
VK_BINDING(46, 0) RWTexture2D<uint> g_surfaceIdentity : register(u38, space0);
VK_BINDING(47, 0) RWTexture2D<uint> g_previousSurfaceIdentity : register(u39, space0);
VK_BINDING(48, 0) RWTexture2D<float4> g_finalResolvedHdr : register(u40, space0);
#include "PathTracingQualityCounters.hlsli"

static const float QualityEpsilon = 1.0e-5f;
static const uint RejectOutOfBounds = 1u << 0u;
static const uint RejectDepth = 1u << 1u;
static const uint RejectNormal = 1u << 2u;
static const uint RejectRoughness = 1u << 3u;
static const uint RejectIdentity = 1u << 4u;

groupshared uint s_surfaceHistoryTested[256];
groupshared uint s_surfaceHistoryAccepted[256];
groupshared uint s_rejectOutOfBounds[256];
groupshared uint s_rejectDepth[256];
groupshared uint s_rejectNormal[256];
groupshared uint s_rejectRoughness[256];
groupshared uint s_rejectIdentity[256];
groupshared uint s_acceptedByDilation[256];
groupshared uint s_taaHistoryTested[256];
groupshared uint s_taaHistoryAccepted[256];
groupshared uint s_disoccludedPixels[256];
groupshared uint s_clampedSamples[256];
groupshared float s_contributionInputEnergy[256];
groupshared float s_contributionOutputEnergy[256];
groupshared float s_contributionClampedEnergy[256];
groupshared uint s_nonFinitePixels[256];

bool IsInside(int2 pixel, uint2 dimensions)
{
    return pixel.x >= 0 && pixel.y >= 0 && pixel.x < (int)dimensions.x && pixel.y < (int)dimensions.y;
}

bool Invalid4(float4 value)
{
    return any(isnan(value)) || any(isinf(value));
}

float3 DecodeNormal(float4 aov)
{
    float3 normal = aov.xyz * 2.0f - 1.0f;
    return dot(normal, normal) > QualityEpsilon ? normalize(normal) : float3(0.0f, 1.0f, 0.0f);
}

float2 ProjectWorldToUv(float3 worldPosition, float4x4 viewProjection)
{
    float4 clip = mul(float4(worldPosition, 1.0f), viewProjection);
    float2 ndc = clip.xy / max(abs(clip.w), QualityEpsilon);
    return float2(ndc.x * 0.5f + 0.5f, 0.5f - ndc.y * 0.5f);
}

float2 BackgroundMotion(uint2 pixel, uint2 dimensions)
{
    float2 uv = (float2(pixel) + 0.5f.xx) / float2(dimensions);
    float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    float4 nearPoint = mul(float4(ndc, 0.0f, 1.0f), g_scene.inverseViewProjection);
    float4 farPoint = mul(float4(ndc, 1.0f, 1.0f), g_scene.inverseViewProjection);
    nearPoint.xyz /= max(abs(nearPoint.w), QualityEpsilon);
    farPoint.xyz /= max(abs(farPoint.w), QualityEpsilon);
    float3 direction = normalize(farPoint.xyz - nearPoint.xyz);
    float3 distantPoint = g_scene.cameraPosition.xyz + direction * 100000.0f;
    return ProjectWorldToUv(distantPoint, g_scene.previousViewProjection) - uv;
}

bool ValidateSurfaceWithReason(
    float4 currentAov0,
    float4 currentAov1,
    float4 currentAov2,
    uint currentIdentity,
    float4 previousAov0,
    float4 previousAov1,
    float4 previousAov2,
    uint previousIdentity,
    out uint rejectReason)
{
    rejectReason = 0u;
    const bool currentHit = currentAov2.w > 0.0f;
    const bool previousHit = previousAov2.w > 0.0f;
    if (currentHit != previousHit)
    {
        // Match FinalTaaCS: stationary alpha/silhouette coverage integrates;
        // moving hit/miss toggles are true reveal events.
        if (g_scene.stabilityOptions.y <= 0.001f)
        {
            return true;
        }
        rejectReason = RejectIdentity;
        return false;
    }
    if (!currentHit)
    {
        return true;
    }
    if (!ValidatePackedSurfaceGroup(currentIdentity, previousIdentity))
    {
        rejectReason = RejectIdentity;
        return false;
    }
    bool samePrimitive = ValidatePackedPrimitiveIdentity(currentIdentity, previousIdentity);

    float normalDot = dot(DecodeNormal(currentAov0), DecodeNormal(previousAov0));
    float expectedPreviousViewZ = max(currentAov0.w + currentAov2.z, QualityEpsilon);
    float depthDelta = abs(expectedPreviousViewZ - previousAov0.w) / max(expectedPreviousViewZ, 1.0f);
    float roughnessDelta = abs(currentAov1.w - previousAov1.w);
    if (depthDelta > g_scene.validationOptions.y) rejectReason |= RejectDepth;
    if (normalDot < g_scene.validationOptions.x) rejectReason |= RejectNormal;
    if (roughnessDelta > g_scene.validationOptions.w) rejectReason |= RejectRoughness;
    // Albedo is deliberately excluded: textured RGB is a shading signal rather
    // than a stable guide. Packed material/instance/primitive identity protects
    // true discontinuities and mirrors FinalTaaCS exactly.
    bool continuousTriangleSeam = !samePrimitive &&
        normalDot >= max(g_scene.validationOptions.x, 0.985f) &&
        depthDelta <= min(g_scene.validationOptions.y, 0.0025f) &&
        roughnessDelta <= min(g_scene.validationOptions.w, 0.025f);
    if (!samePrimitive && !continuousTriangleSeam) rejectReason |= RejectIdentity;
    return rejectReason == 0u;
}

bool EvaluateHistoryAcceptance(
    uint2 pixel,
    uint2 dimensions,
    out bool acceptedByDilation,
    out uint rejectReason)
{
    acceptedByDilation = false;
    rejectReason = 0u;
    uint historyDomains = (uint)round(g_scene.environmentOptions.w);
    if ((historyDomains & (HISTORY_DOMAIN_SURFACE | HISTORY_DOMAIN_TAA)) !=
        (HISTORY_DOMAIN_SURFACE | HISTORY_DOMAIN_TAA))
    {
        return false;
    }

    float4 currentAov0 = g_denoiseAov0[pixel];
    float4 currentAov1 = g_denoiseAov1[pixel];
    float4 currentAov2 = g_denoiseAov2[pixel];
    uint currentIdentity = g_surfaceIdentity[pixel];
    bool hit = currentAov2.w > 0.0f;
    float2 motion = hit ? currentAov2.xy : BackgroundMotion(pixel, dimensions);
    float2 jitterDelta = g_scene.jitterOptions.xy - g_scene.jitterOptions.zw;
    float2 historyPosition = float2(pixel) + motion * float2(dimensions) + jitterDelta;
    int2 basePixel = int2(floor(historyPosition));
    float2 fraction = frac(historyPosition);
    float totalWeight = 0.0f;
    uint observedReasons = 0u;
    uint inBoundsTaps = 0u;

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
            inBoundsTaps++;
            uint2 previousPixel = uint2(candidate);
            uint tapReason;
            bool valid = ValidateSurfaceWithReason(
                currentAov0, currentAov1, currentAov2, currentIdentity,
                g_previousDenoiseAov0[previousPixel],
                g_previousDenoiseAov1[previousPixel],
                g_previousDenoiseAov2[previousPixel],
                g_previousSurfaceIdentity[previousPixel],
                tapReason);
            observedReasons |= tapReason;
            if (valid)
            {
                totalWeight += (x == 0 ? 1.0f - fraction.x : fraction.x) *
                    (y == 0 ? 1.0f - fraction.y : fraction.y);
            }
        }
    }
    if (totalWeight > QualityEpsilon)
    {
        return true;
    }

    int2 centerPixel = int2(round(historyPosition));
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
            uint2 previousPixel = uint2(candidate);
            uint tapReason;
            if (ValidateSurfaceWithReason(
                currentAov0, currentAov1, currentAov2, currentIdentity,
                g_previousDenoiseAov0[previousPixel],
                g_previousDenoiseAov1[previousPixel],
                g_previousDenoiseAov2[previousPixel],
                g_previousSurfaceIdentity[previousPixel],
                tapReason))
            {
                acceptedByDilation = true;
                return true;
            }
            observedReasons |= tapReason;
        }
    }

    rejectReason = inBoundsTaps == 0u ? RejectOutOfBounds : observedReasons;
    if (rejectReason == 0u)
    {
        rejectReason = RejectIdentity;
    }
    return false;
}

void ReduceLane(uint target, uint source)
{
    s_surfaceHistoryTested[target] += s_surfaceHistoryTested[source];
    s_surfaceHistoryAccepted[target] += s_surfaceHistoryAccepted[source];
    s_rejectOutOfBounds[target] += s_rejectOutOfBounds[source];
    s_rejectDepth[target] += s_rejectDepth[source];
    s_rejectNormal[target] += s_rejectNormal[source];
    s_rejectRoughness[target] += s_rejectRoughness[source];
    s_rejectIdentity[target] += s_rejectIdentity[source];
    s_acceptedByDilation[target] += s_acceptedByDilation[source];
    s_taaHistoryTested[target] += s_taaHistoryTested[source];
    s_taaHistoryAccepted[target] += s_taaHistoryAccepted[source];
    s_disoccludedPixels[target] += s_disoccludedPixels[source];
    s_clampedSamples[target] += s_clampedSamples[source];
    s_contributionInputEnergy[target] += s_contributionInputEnergy[source];
    s_contributionOutputEnergy[target] += s_contributionOutputEnergy[source];
    s_contributionClampedEnergy[target] += s_contributionClampedEnergy[source];
    s_nonFinitePixels[target] += s_nonFinitePixels[source];
}

[numthreads(16, 16, 1)]
void QualityCountersCS(
    uint3 dispatchThreadId : SV_DispatchThreadID,
    uint3 groupId : SV_GroupID,
    uint groupIndex : SV_GroupIndex)
{
    uint2 dimensions = (uint2)round(g_scene.rayOptions.zw);
    uint2 pixel = dispatchThreadId.xy;
    bool inBounds = all(pixel < dimensions);

    uint surfaceTested = 0u;
    uint surfaceAccepted = 0u;
    uint rejectOutOfBounds = 0u;
    uint rejectDepth = 0u;
    uint rejectNormal = 0u;
    uint rejectRoughness = 0u;
    uint rejectIdentity = 0u;
    uint dilationAccepted = 0u;
    uint taaTested = 0u;
    uint taaAccepted = 0u;
    uint disoccluded = 0u;
    uint clampedSample = 0u;
    float inputEnergy = 0.0f;
    float outputEnergy = 0.0f;
    float clampedEnergy = 0.0f;
    uint nonFinite = 0u;

    if (inBounds)
    {
        uint resolvedWidth;
        uint resolvedHeight;
        g_finalResolvedHdr.GetDimensions(resolvedWidth, resolvedHeight);
        bool taaActive = resolvedWidth == dimensions.x && resolvedHeight == dimensions.y &&
            (uint)round(g_scene.debugOptions.x) < 41u;
        bool acceptedByDilation;
        uint rejectReason;
        bool accepted = EvaluateHistoryAcceptance(pixel, dimensions, acceptedByDilation, rejectReason);
        bool surface = g_denoiseAov2[pixel].w > 0.0f;
        uint historyDomains = (uint)round(g_scene.environmentOptions.w);
        bool historyDomainValid = (historyDomains & (HISTORY_DOMAIN_SURFACE | HISTORY_DOMAIN_TAA)) ==
            (HISTORY_DOMAIN_SURFACE | HISTORY_DOMAIN_TAA);
        surfaceTested = surface && historyDomainValid ? 1u : 0u;
        surfaceAccepted = surface && accepted ? 1u : 0u;
        dilationAccepted = surfaceAccepted != 0u && acceptedByDilation ? 1u : 0u;
        if (surfaceTested != 0u && surfaceAccepted == 0u)
        {
            rejectOutOfBounds = (rejectReason & RejectOutOfBounds) != 0u ? 1u : 0u;
            rejectDepth = (rejectReason & RejectDepth) != 0u ? 1u : 0u;
            rejectNormal = (rejectReason & RejectNormal) != 0u ? 1u : 0u;
            rejectRoughness = (rejectReason & RejectRoughness) != 0u ? 1u : 0u;
            rejectIdentity = (rejectReason & RejectIdentity) != 0u ? 1u : 0u;
        }
        uint packedContribution = g_qualityContribution[pixel];
        bool directTaaDecision = QualityTaaDecisionWasRecorded(packedContribution);
        bool finalTaaAccepted = directTaaDecision
            ? QualityTaaDecisionWasAccepted(packedContribution)
            : accepted;
        taaTested = taaActive ? 1u : 0u;
        taaAccepted = taaActive && finalTaaAccepted ? 1u : 0u;
        disoccluded = taaActive && !finalTaaAccepted ? 1u : 0u;

        float2 contributionEnergy = UnpackQualityContributionEnergy(packedContribution);
        bool invalidContribution = any(isnan(contributionEnergy)) || any(isinf(contributionEnergy));
        if (!invalidContribution)
        {
            inputEnergy = max(contributionEnergy.x, 0.0f);
            outputEnergy = max(contributionEnergy.y, 0.0f);
            clampedEnergy = max(inputEnergy - outputEnergy, 0.0f);
            float compressionTolerance = max(inputEnergy * 1.0e-3f, 1.0e-4f);
            clampedSample = clampedEnergy > compressionTolerance ? 1u : 0u;
        }

        bool invalidPixel = invalidContribution ||
            Invalid4(g_output[pixel]) ||
            Invalid4(g_denoiseAov0[pixel]) ||
            Invalid4(g_denoiseAov1[pixel]) ||
            Invalid4(g_denoiseAov2[pixel]) ||
            Invalid4(g_signalDirect[pixel]) ||
            Invalid4(g_signalIndirect[pixel]) ||
            Invalid4(g_signalResidual[pixel]) ||
            Invalid4(g_postDenoiseHdr[pixel]);
        if (resolvedWidth == dimensions.x && resolvedHeight == dimensions.y)
        {
            invalidPixel = invalidPixel || Invalid4(g_finalResolvedHdr[pixel]);
        }
        nonFinite = invalidPixel ? 1u : 0u;
    }

    s_surfaceHistoryTested[groupIndex] = surfaceTested;
    s_surfaceHistoryAccepted[groupIndex] = surfaceAccepted;
    s_rejectOutOfBounds[groupIndex] = rejectOutOfBounds;
    s_rejectDepth[groupIndex] = rejectDepth;
    s_rejectNormal[groupIndex] = rejectNormal;
    s_rejectRoughness[groupIndex] = rejectRoughness;
    s_rejectIdentity[groupIndex] = rejectIdentity;
    s_acceptedByDilation[groupIndex] = dilationAccepted;
    s_taaHistoryTested[groupIndex] = taaTested;
    s_taaHistoryAccepted[groupIndex] = taaAccepted;
    s_disoccludedPixels[groupIndex] = disoccluded;
    s_clampedSamples[groupIndex] = clampedSample;
    s_contributionInputEnergy[groupIndex] = inputEnergy;
    s_contributionOutputEnergy[groupIndex] = outputEnergy;
    s_contributionClampedEnergy[groupIndex] = clampedEnergy;
    s_nonFinitePixels[groupIndex] = nonFinite;
    GroupMemoryBarrierWithGroupSync();

    [unroll]
    for (uint stride = 128u; stride > 0u; stride >>= 1u)
    {
        if (groupIndex < stride)
        {
            ReduceLane(groupIndex, groupIndex + stride);
        }
        GroupMemoryBarrierWithGroupSync();
    }

    if (groupIndex == 0u)
    {
        QualityCounterTileV1 tile;
        tile.surfaceHistoryTested = s_surfaceHistoryTested[0];
        tile.surfaceHistoryAccepted = s_surfaceHistoryAccepted[0];
        tile.rejectOutOfBounds = s_rejectOutOfBounds[0];
        tile.rejectDepth = s_rejectDepth[0];
        tile.rejectNormal = s_rejectNormal[0];
        tile.rejectRoughness = s_rejectRoughness[0];
        tile.rejectIdentity = s_rejectIdentity[0];
        tile.acceptedByDilation = s_acceptedByDilation[0];
        tile.taaHistoryTested = s_taaHistoryTested[0];
        tile.taaHistoryAccepted = s_taaHistoryAccepted[0];
        tile.disoccludedPixels = s_disoccludedPixels[0];
        tile.clampedSamples = s_clampedSamples[0];
        tile.contributionInputEnergy = s_contributionInputEnergy[0];
        tile.contributionOutputEnergy = s_contributionOutputEnergy[0];
        tile.contributionClampedEnergy = s_contributionClampedEnergy[0];
        tile.nonFinitePixels = s_nonFinitePixels[0];
        uint tileCountX = (dimensions.x + 15u) / 16u;
        g_qualityCounters[groupId.y * tileCountX + groupId.x] = tile;
    }
}
