#ifndef PATH_TRACING_SCENE_CONSTANTS_HLSLI
#define PATH_TRACING_SCENE_CONSTANTS_HLSLI

// Canonical C++/HLSL constant-buffer ordering. Keep this in lockstep with
// D3D12PathTracingBackend::SceneConstantBuffer; C++ size/offset assertions
// guard the fields consumed by every DXR, denoise, NRD, and TAA pass.
struct SceneConstants
{
    row_major float4x4 inverseViewProjection;
    row_major float4x4 viewProjection;
    row_major float4x4 previousViewProjection;
    float4 cameraPosition;
    float4 lightDirection;
    float4 lightColor;
    float4 debugOptions;
    float4 skyColor;
    float4 skyHorizonColor;
    float4 skyZenithColor;
    float4 skyGroundColor;
    float4 skyOptions;
    float4 rayOptions;
    uint4 frameOptions;
    float4 giOptions;
    float4 pathOptions;
    float4 restirOptions;
    float4 restirDiOptions;
    float4 lightOptions;
    float4 environmentOptions;
    float4 denoiseOptions;
    float4 denoiseOptions2;
    float4 jitterOptions;
    float4 reconstructionOptions;
    float4 validationOptions;
    float4 atrousOptions;
    float4 adaptiveOptions;
    float4 restirStabilityOptions;
    float4 signalDenoiseOptions;
    float4 denoisePassOptions;
    float4 stabilityOptions;
    float4 viewOptions;
    float4 materialFocusOptions;
    float4 performanceOptions;
    float4 postProcessOptions;
    float4 unifiedLightOptions;
    float4 renderOutputOptions;
    // Previous-frame camera data is appended to preserve the offsets of the
    // established ABI above. GI spatial reuse reads previous-frame guides and
    // must reconstruct the donor receiver in that same frame.
    row_major float4x4 previousInverseViewProjection;
    float4 previousCameraPosition;
    row_major float4x4 environmentLightToWorld;
    row_major float4x4 environmentWorldToLight;
    float4 environmentTint;
};

static const uint HISTORY_DOMAIN_SURFACE = 1u << 0u;
static const uint HISTORY_DOMAIN_LIGHTING = 1u << 1u;
static const uint HISTORY_DOMAIN_DENOISER = 1u << 2u;
static const uint HISTORY_DOMAIN_TAA = 1u << 3u;

// Packed primary-surface identity layout:
//   [31:10] surface group (instance + geometry + material hash)
//   [ 9: 4] primitive signature
//   [ 3: 0] quantized alpha coverage
// Zero is reserved for sky/background. Splitting group and primitive lets the
// final TAA recognize a geometrically continuous triangle seam without making
// ReSTIR or denoiser history less strict.
static const uint SURFACE_IDENTITY_COVERAGE_BITS = 4u;
static const uint SURFACE_IDENTITY_COVERAGE_MASK = (1u << SURFACE_IDENTITY_COVERAGE_BITS) - 1u;
static const uint SURFACE_IDENTITY_PRIMITIVE_BITS = 6u;
static const uint SURFACE_IDENTITY_PRIMITIVE_MASK = (1u << SURFACE_IDENTITY_PRIMITIVE_BITS) - 1u;
static const uint SURFACE_IDENTITY_GROUP_SHIFT = SURFACE_IDENTITY_COVERAGE_BITS + SURFACE_IDENTITY_PRIMITIVE_BITS;

bool ValidatePackedSurfaceGroup(uint currentPacked, uint previousPacked)
{
    if (currentPacked == 0u || previousPacked == 0u)
    {
        return false;
    }

    uint currentGroup = currentPacked >> SURFACE_IDENTITY_GROUP_SHIFT;
    uint previousGroup = previousPacked >> SURFACE_IDENTITY_GROUP_SHIFT;
    uint currentCoverage = currentPacked & SURFACE_IDENTITY_COVERAGE_MASK;
    uint previousCoverage = previousPacked & SURFACE_IDENTITY_COVERAGE_MASK;
    uint coverageDelta = currentCoverage > previousCoverage
        ? currentCoverage - previousCoverage
        : previousCoverage - currentCoverage;
    return currentGroup == previousGroup && coverageDelta <= 2u;
}

bool ValidatePackedPrimitiveIdentity(uint currentPacked, uint previousPacked)
{
    uint currentPrimitive = (currentPacked >> SURFACE_IDENTITY_COVERAGE_BITS) & SURFACE_IDENTITY_PRIMITIVE_MASK;
    uint previousPrimitive = (previousPacked >> SURFACE_IDENTITY_COVERAGE_BITS) & SURFACE_IDENTITY_PRIMITIVE_MASK;
    return currentPrimitive == previousPrimitive;
}

bool ValidatePackedSurfaceIdentity(uint currentPacked, uint previousPacked)
{
    return ValidatePackedSurfaceGroup(currentPacked, previousPacked) &&
        ValidatePackedPrimitiveIdentity(currentPacked, previousPacked);
}

#endif
