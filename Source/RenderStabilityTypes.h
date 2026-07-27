#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace rb
{
enum class FrameChangeMask : std::uint32_t
{
    None = 0,
    CameraMotion = 1u << 0,
    CameraCut = 1u << 1,
    Projection = 1u << 2,
    Resolution = 1u << 3,
    Material = 1u << 4,
    Light = 1u << 5,
    Hdri = 1u << 6,
    Geometry = 1u << 7,
    Backend = 1u << 8,
    QualityProfile = 1u << 9,
    DenoiserSettings = 1u << 10,
    ManualReset = 1u << 11,
    View = 1u << 12,
};

enum class HistoryDomain : std::uint32_t
{
    None = 0,
    Surface = 1u << 0,
    Lighting = 1u << 1,
    Denoiser = 1u << 2,
    Taa = 1u << 3,
    ReferenceAccumulation = 1u << 4,
    Realtime = (1u << 0) | (1u << 1) | (1u << 2) | (1u << 3),
    All = (1u << 0) | (1u << 1) | (1u << 2) | (1u << 3) | (1u << 4),
};

constexpr FrameChangeMask operator|(FrameChangeMask left, FrameChangeMask right)
{
    return static_cast<FrameChangeMask>(static_cast<std::uint32_t>(left) | static_cast<std::uint32_t>(right));
}

constexpr FrameChangeMask& operator|=(FrameChangeMask& left, FrameChangeMask right)
{
    left = left | right;
    return left;
}

constexpr HistoryDomain operator|(HistoryDomain left, HistoryDomain right)
{
    return static_cast<HistoryDomain>(static_cast<std::uint32_t>(left) | static_cast<std::uint32_t>(right));
}

constexpr HistoryDomain operator&(HistoryDomain left, HistoryDomain right)
{
    return static_cast<HistoryDomain>(static_cast<std::uint32_t>(left) & static_cast<std::uint32_t>(right));
}

constexpr HistoryDomain operator~(HistoryDomain value)
{
    return static_cast<HistoryDomain>(~static_cast<std::uint32_t>(value));
}

constexpr HistoryDomain& operator|=(HistoryDomain& left, HistoryDomain right)
{
    left = left | right;
    return left;
}

constexpr HistoryDomain& operator&=(HistoryDomain& left, HistoryDomain right)
{
    left = left & right;
    return left;
}

constexpr bool HasAny(FrameChangeMask value, FrameChangeMask flags)
{
    return (static_cast<std::uint32_t>(value) & static_cast<std::uint32_t>(flags)) != 0;
}

constexpr bool HasAny(HistoryDomain value, HistoryDomain flags)
{
    return (static_cast<std::uint32_t>(value) & static_cast<std::uint32_t>(flags)) != 0;
}

constexpr bool HasAll(HistoryDomain value, HistoryDomain flags)
{
    return (static_cast<std::uint32_t>(value) & static_cast<std::uint32_t>(flags)) == static_cast<std::uint32_t>(flags);
}

// Centralized invalidation policy. Callers should describe the change, not
// duplicate the reset table at each mutation site.
constexpr HistoryDomain HistoryDomainsForChange(FrameChangeMask changes)
{
    HistoryDomain domains = HistoryDomain::None;
    if (HasAny(changes, FrameChangeMask::CameraMotion))
    {
        domains |= HistoryDomain::ReferenceAccumulation;
    }
    if (HasAny(changes, FrameChangeMask::Material | FrameChangeMask::Light | FrameChangeMask::Hdri |
        FrameChangeMask::Backend | FrameChangeMask::QualityProfile))
    {
        domains |= HistoryDomain::Lighting | HistoryDomain::Denoiser | HistoryDomain::Taa |
            HistoryDomain::ReferenceAccumulation;
    }
    if (HasAny(changes, FrameChangeMask::DenoiserSettings))
    {
        domains |= HistoryDomain::Denoiser | HistoryDomain::Taa;
    }
    if (HasAny(changes, FrameChangeMask::View))
    {
        domains |= HistoryDomain::Denoiser | HistoryDomain::Taa |
            HistoryDomain::ReferenceAccumulation;
    }
    if (HasAny(changes, FrameChangeMask::CameraCut | FrameChangeMask::Projection |
        FrameChangeMask::Resolution | FrameChangeMask::Geometry | FrameChangeMask::ManualReset))
    {
        domains |= HistoryDomain::All;
    }
    return domains;
}

static_assert(HistoryDomainsForChange(FrameChangeMask::CameraMotion) == HistoryDomain::ReferenceAccumulation);
static_assert(HistoryDomainsForChange(FrameChangeMask::CameraCut) == HistoryDomain::All);
static_assert(HistoryDomainsForChange(FrameChangeMask::Projection) == HistoryDomain::All);
static_assert(HistoryDomainsForChange(FrameChangeMask::Resolution) == HistoryDomain::All);
static_assert(HistoryDomainsForChange(FrameChangeMask::Geometry) == HistoryDomain::All);
static_assert(HistoryDomainsForChange(FrameChangeMask::Material) ==
    (HistoryDomain::Lighting | HistoryDomain::Denoiser | HistoryDomain::Taa | HistoryDomain::ReferenceAccumulation));
static_assert(HistoryDomainsForChange(FrameChangeMask::Light) ==
    (HistoryDomain::Lighting | HistoryDomain::Denoiser | HistoryDomain::Taa | HistoryDomain::ReferenceAccumulation));
static_assert(HistoryDomainsForChange(FrameChangeMask::Hdri) ==
    (HistoryDomain::Lighting | HistoryDomain::Denoiser | HistoryDomain::Taa | HistoryDomain::ReferenceAccumulation));
static_assert(HistoryDomainsForChange(FrameChangeMask::Backend | FrameChangeMask::QualityProfile) ==
    (HistoryDomain::Lighting | HistoryDomain::Denoiser | HistoryDomain::Taa | HistoryDomain::ReferenceAccumulation));
static_assert(HistoryDomainsForChange(FrameChangeMask::DenoiserSettings) ==
    (HistoryDomain::Denoiser | HistoryDomain::Taa));
static_assert(HistoryDomainsForChange(FrameChangeMask::View) ==
    (HistoryDomain::Denoiser | HistoryDomain::Taa | HistoryDomain::ReferenceAccumulation));

struct FrameRevisions
{
    std::uint64_t scene = 0;
    std::uint64_t geometry = 0;
    std::uint64_t material = 0;
    std::uint64_t light = 0;
    std::uint64_t hdri = 0;
    std::uint64_t backend = 0;
    std::uint64_t qualityProfile = 0;
};

struct FrameState
{
    std::uint64_t frameNumber = 0;
    std::uint32_t progressiveSampleCount = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    FrameChangeMask changes = FrameChangeMask::None;
    HistoryDomain validHistoryDomains = HistoryDomain::None;
    bool cameraCut = false;
    std::array<float, 3> currentCameraPosition{};
    std::array<float, 3> previousCameraPosition{};
    float currentYaw = 0.0f;
    float previousYaw = 0.0f;
    float currentPitch = 0.0f;
    float previousPitch = 0.0f;
    std::array<float, 2> currentJitterPixels{};
    std::array<float, 2> previousJitterPixels{};
    FrameRevisions revisions{};
};

// Per-pixel public signal contracts. The renderer may store these in separate
// textures; these structures define semantics rather than a required texture pack.
struct alignas(16) SurfaceGuides
{
    std::array<float, 3> geometricNormal{};
    float linearViewZ = 0.0f;
    std::array<float, 3> shadingNormal{};
    float roughness = 1.0f;
    std::array<float, 3> albedo{};
    float metallic = 0.0f;
    std::array<float, 2> motionUv{};
    float previousMinusCurrentViewZ = 0.0f;
    // 0 is sky/miss, positive values are the primary ray hit distance.
    float primaryHitT = 0.0f;
    float coverage = 1.0f;
    float reactive = 0.0f;
    float rayFootprint = 0.0f;
    std::uint32_t materialId = 0;
    std::uint32_t instanceId = 0;
    std::uint32_t primitiveId = 0;
    std::array<std::uint32_t, 2> reserved{};
};

struct alignas(16) LightingSignals
{
    // RGB radiance plus actual first-secondary-hit distance in W.
    std::array<float, 4> diffuseRadianceHitDistance{};
    std::array<float, 4> specularRadianceHitDistance{};
    std::array<float, 4> emissionSky{};
    float diffuseConfidence = 0.0f;
    float specularConfidence = 0.0f;
    std::array<float, 2> reserved{};
};

struct RayBudgetState
{
    std::uint32_t requestedSpp = 1;
    std::uint32_t effectiveSpp = 1;
    std::uint32_t requestedBounces = 2;
    std::uint32_t effectiveBounces = 2;
    std::uint32_t overBudgetFrames = 0;
    std::uint32_t underBudgetFrames = 0;
    bool moving = false;
    bool extraSampleQuotaEnabled = false;
    double targetGpuMs = 14.5;
    double lastGpuMs = 0.0;
};

static_assert(std::is_standard_layout_v<FrameState>);
static_assert(std::is_standard_layout_v<SurfaceGuides>);
static_assert(std::is_standard_layout_v<LightingSignals>);
static_assert(alignof(SurfaceGuides) == 16);
static_assert(sizeof(SurfaceGuides) == 96);
static_assert(offsetof(SurfaceGuides, motionUv) == 48);
static_assert(offsetof(SurfaceGuides, primaryHitT) == 60);
static_assert(offsetof(SurfaceGuides, materialId) == 76);
static_assert(alignof(LightingSignals) == 16);
}
