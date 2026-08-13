#ifndef PATH_TRACING_DEBUG_VIEWS_HLSLI
#define PATH_TRACING_DEBUG_VIEWS_HLSLI

// Shared deterministic visualization for primary/path evidence. Review
// capture and the interactive renderer use the same numeric view identifiers.
float3 SelectPrimaryDebugView(
    uint debugMode,
    float3 finalRadiance,
    bool hit,
    float3 baseColor,
    float3 worldNormal,
    float3 normalTexture,
    float roughness,
    float metallic,
    float3 emissive,
    float hitDistance,
    float3 directSignal,
    float3 indirectSignal,
    float averageBounces,
    float maximumBounces,
    float3 sky)
{
    if (debugMode == 1u) return baseColor;
    if (debugMode == 2u) return hit ? worldNormal * 0.5f + 0.5f : 0.0f.xxx;
    if (debugMode == 3u) return normalTexture;
    if (debugMode == 4u) return roughness.xxx;
    if (debugMode == 5u) return metallic.xxx;
    if (debugMode == 6u) return emissive;
    if (debugMode == 7u) return hit ? saturate(hitDistance / 250.0f).xxx : 0.0f.xxx;
    if (debugMode == 8u) return directSignal;
    if (debugMode == 9u) return indirectSignal;
    if (debugMode == 10u) return (averageBounces / max(maximumBounces, 1.0f)).xxx;
    if (debugMode == 12u) return sky;
    return finalRadiance;
}

#endif
