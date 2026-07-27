#pragma once

#include <cstdint>
#include <string_view>

namespace rb
{
enum class QualityProfile : std::uint32_t
{
    InteractiveGame,
    SharpPreview,
    ReferenceStill,
};

enum class RestirBackend : std::uint32_t
{
    Rtxdi,
    Off,
};

enum class SecondaryShadingRate : std::uint32_t
{
    Auto,
    Full,
    AdaptiveHalf,
};

enum class CameraHistoryMode : std::uint32_t
{
    Auto,
    Preserve,
    Reset,
};

struct RayBudgetSettings
{
    std::uint32_t movingSpp = 1;
    std::uint32_t movingBounces = 2;
    std::uint32_t staticBaseSpp = 1;
    std::uint32_t staticMaxSpp = 2;
    std::uint32_t staticBounces = 4;
    std::uint32_t settleFrames = 8;
    float targetGpuMs = 14.5f;
};

struct QualitySettings
{
    QualityProfile qualityProfile = QualityProfile::InteractiveGame;
    RestirBackend restirBackend = RestirBackend::Rtxdi;
    SecondaryShadingRate secondaryShadingRate = SecondaryShadingRate::Auto;
    RayBudgetSettings rayBudget;
    bool finalTaa = true;
    float sharpenStrength = 0.15f;
    std::uint32_t referenceSpp = 4096;
};

constexpr std::string_view QualityProfileName(QualityProfile profile)
{
    switch (profile)
    {
    case QualityProfile::InteractiveGame:
        return "interactive_game";
    case QualityProfile::SharpPreview:
        return "sharp_preview";
    case QualityProfile::ReferenceStill:
        return "reference_still";
    }
    return "interactive_game";
}

inline bool TryParseQualityProfile(std::string_view name, QualityProfile& profile)
{
    if (name == "interactive_game" || name == "interactive_stable")
    {
        profile = QualityProfile::InteractiveGame;
        return true;
    }
    if (name == "sharp_preview")
    {
        profile = QualityProfile::SharpPreview;
        return true;
    }
    if (name == "reference_still" || name == "still_capture")
    {
        profile = QualityProfile::ReferenceStill;
        return true;
    }
    return false;
}

constexpr std::string_view RestirBackendName(RestirBackend backend)
{
    switch (backend)
    {
    case RestirBackend::Rtxdi:
        return "rtxdi";
    case RestirBackend::Off:
        return "off";
    }
    return "off";
}

inline bool TryParseRestirBackend(std::string_view name, RestirBackend& backend)
{
    if (name == "rtxdi")
    {
        backend = RestirBackend::Rtxdi;
        return true;
    }
    if (name == "off")
    {
        backend = RestirBackend::Off;
        return true;
    }
    return false;
}

constexpr SecondaryShadingRate DefaultSecondaryShadingRate(QualityProfile profile)
{
    return profile == QualityProfile::InteractiveGame
        ? SecondaryShadingRate::Auto
        : SecondaryShadingRate::Full;
}

constexpr std::string_view SecondaryShadingRateName(SecondaryShadingRate rate)
{
    switch (rate)
    {
    case SecondaryShadingRate::Auto:
        return "auto";
    case SecondaryShadingRate::Full:
        return "full";
    case SecondaryShadingRate::AdaptiveHalf:
        return "adaptive_half";
    }
    return "auto";
}

inline bool TryParseSecondaryShadingRate(std::string_view name, SecondaryShadingRate& rate)
{
    if (name == "auto")
    {
        rate = SecondaryShadingRate::Auto;
        return true;
    }
    if (name == "full")
    {
        rate = SecondaryShadingRate::Full;
        return true;
    }
    if (name == "adaptive_half")
    {
        rate = SecondaryShadingRate::AdaptiveHalf;
        return true;
    }
    return false;
}

constexpr std::string_view CameraHistoryModeName(CameraHistoryMode mode)
{
    switch (mode)
    {
    case CameraHistoryMode::Auto:
        return "auto";
    case CameraHistoryMode::Preserve:
        return "preserve";
    case CameraHistoryMode::Reset:
        return "reset";
    }
    return "auto";
}

inline bool TryParseCameraHistoryMode(std::string_view name, CameraHistoryMode& mode)
{
    if (name == "auto")
    {
        mode = CameraHistoryMode::Auto;
        return true;
    }
    if (name == "preserve")
    {
        mode = CameraHistoryMode::Preserve;
        return true;
    }
    if (name == "reset")
    {
        mode = CameraHistoryMode::Reset;
        return true;
    }
    return false;
}
}
