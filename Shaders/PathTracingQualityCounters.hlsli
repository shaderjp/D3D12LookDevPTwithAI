#ifndef PATH_TRACING_QUALITY_COUNTERS_HLSLI
#define PATH_TRACING_QUALITY_COUNTERS_HLSLI

// Keep this byte-for-byte compatible with benchmark::QualityCounterTileV1.
// A separate 16x16 compute pass performs the group-local reduction, so only
// one lane writes each 64-byte tile record and no global float atomics are
// required.
struct QualityCounterTileV1
{
    uint surfaceHistoryTested;
    uint surfaceHistoryAccepted;
    uint rejectOutOfBounds;
    uint rejectDepth;

    uint rejectNormal;
    uint rejectRoughness;
    uint rejectIdentity;
    uint acceptedByDilation;

    uint taaHistoryTested;
    uint taaHistoryAccepted;
    uint disoccludedPixels;
    uint clampedSamples;

    float contributionInputEnergy;
    float contributionOutputEnergy;
    float contributionClampedEnergy;
    uint nonFinitePixels;

    uint primaryRays;
    uint secondaryRays;
    uint shadowRays;
    uint diVisibilityRays;

    uint giVisibilityRays;
    uint ptVisibilityRays;
    uint anyHitInvocations;
    uint reserved;
};

VK_BINDING(49, 0) RWStructuredBuffer<QualityCounterTileV1> g_qualityCounters : register(u41, space0);
VK_BINDING(50, 0) RWTexture2D<uint> g_qualityContribution : register(u42, space0);

static const uint QualityTaaTestedBit = 0x00008000u;
static const uint QualityTaaAcceptedBit = 0x80000000u;
static const uint QualityRayPrimary = 0u;
static const uint QualityRaySecondary = 1u;
static const uint QualityRayShadow = 2u;
static const uint QualityRayDiVisibility = 3u;
static const uint QualityRayGiVisibility = 4u;
static const uint QualityRayPtVisibility = 5u;
static const uint QualityRayAnyHit = 6u;

void RecordQualityRay(
    uint2 pixel,
    uint2 renderDimensions,
    uint rayKind)
{
    if (g_scene.performanceOptions.z < 0.5f ||
        any(pixel >= renderDimensions))
    {
        return;
    }
    uint tileCountX = (renderDimensions.x + 15u) / 16u;
    uint tileIndex =
        (pixel.y >> 4u) * tileCountX + (pixel.x >> 4u);
    uint ignored;
    if (rayKind == QualityRayPrimary)
        InterlockedAdd(g_qualityCounters[tileIndex].primaryRays, 1u, ignored);
    else if (rayKind == QualityRaySecondary)
        InterlockedAdd(g_qualityCounters[tileIndex].secondaryRays, 1u, ignored);
    else if (rayKind == QualityRayShadow)
        InterlockedAdd(g_qualityCounters[tileIndex].shadowRays, 1u, ignored);
    else if (rayKind == QualityRayDiVisibility)
        InterlockedAdd(g_qualityCounters[tileIndex].diVisibilityRays, 1u, ignored);
    else if (rayKind == QualityRayGiVisibility)
        InterlockedAdd(g_qualityCounters[tileIndex].giVisibilityRays, 1u, ignored);
    else if (rayKind == QualityRayPtVisibility)
        InterlockedAdd(g_qualityCounters[tileIndex].ptVisibilityRays, 1u, ignored);
    else if (rayKind == QualityRayAnyHit)
        InterlockedAdd(g_qualityCounters[tileIndex].anyHitInvocations, 1u, ignored);
}

// The diagnostic texture is full resolution only during a benchmark. Normal
// rendering binds a descriptor-valid 1x1 placeholder, and therefore pays no
// full-frame memory or write cost.
bool QualityContributionEnabled(uint2 renderDimensions)
{
    uint width;
    uint height;
    g_qualityContribution.GetDimensions(width, height);
    return width == renderDimensions.x && height == renderDimensions.y;
}

uint PackQualityContributionEnergy(float inputEnergy, float outputEnergy)
{
    if (isnan(inputEnergy) || isinf(inputEnergy) || isnan(outputEnergy) || isinf(outputEnergy))
    {
        // Canonical half NaNs let the validation pass count the originating
        // pixel without allowing a NaN to enter the tile energy reduction.
        return 0x7e007e00u;
    }
    // Half-encode log2(1+x), not x. This retains precision near the normal
    // contribution range while representing extreme fireflies without
    // saturating at 65504 before the CPU double-precision reduction.
    float encodedInput = min(log2(1.0f + max(inputEnergy, 0.0f)), 120.0f);
    float encodedOutput = min(log2(1.0f + max(outputEnergy, 0.0f)), 120.0f);
    return (f32tof16(encodedInput) & 0xffffu) | ((f32tof16(encodedOutput) & 0xffffu) << 16u);
}

float2 UnpackQualityContributionEnergy(uint packed)
{
    // The two half sign bits are reserved for the optional direct FinalTaaCS
    // decision hook. Encoded log energy is always non-negative.
    float encodedInput = f16tof32(packed & 0x7fffu);
    float encodedOutput = f16tof32((packed >> 16u) & 0x7fffu);
    return float2(exp2(encodedInput) - 1.0f, exp2(encodedOutput) - 1.0f);
}

void StoreQualityTaaDecision(uint2 pixel, uint2 renderDimensions, bool tested, bool accepted)
{
    if (!QualityContributionEnabled(renderDimensions))
    {
        return;
    }
    uint packed = g_qualityContribution[pixel] & ~(QualityTaaTestedBit | QualityTaaAcceptedBit);
    if (tested) packed |= QualityTaaTestedBit;
    if (accepted) packed |= QualityTaaAcceptedBit;
    g_qualityContribution[pixel] = packed;
}

bool QualityTaaDecisionWasRecorded(uint packed)
{
    return (packed & QualityTaaTestedBit) != 0u;
}

bool QualityTaaDecisionWasAccepted(uint packed)
{
    return (packed & QualityTaaAcceptedBit) != 0u;
}

#endif
