#pragma once

#include "QualitySettings.h"
#include "SimpleJson.h"

#include <cmath>
#include <sstream>
#include <string>

namespace rb
{
namespace detail
{
inline bool ReadUnsignedQualitySetting(
    const cld::JsonValue& object,
    const char* name,
    std::uint32_t minimum,
    std::uint32_t maximum,
    std::uint32_t& value,
    std::string& diagnostics)
{
    const cld::JsonValue* member = cld::FindMember(object, name);
    if (!member)
    {
        return true;
    }
    if (member->type != cld::JsonValue::Type::Number || !std::isfinite(member->number) ||
        std::floor(member->number) != member->number || member->number < minimum || member->number > maximum)
    {
        diagnostics = std::string("quality.") + name + " is outside the supported integer range.";
        return false;
    }
    value = static_cast<std::uint32_t>(member->number);
    return true;
}

inline bool ReadFloatQualitySetting(
    const cld::JsonValue& object,
    const char* name,
    float minimum,
    float maximum,
    float& value,
    std::string& diagnostics)
{
    const cld::JsonValue* member = cld::FindMember(object, name);
    if (!member)
    {
        return true;
    }
    if (member->type != cld::JsonValue::Type::Number || !std::isfinite(member->number) ||
        member->number < minimum || member->number > maximum)
    {
        diagnostics = std::string("quality.") + name + " is outside the supported range.";
        return false;
    }
    value = static_cast<float>(member->number);
    return true;
}
}

// Parses either a project file's top-level "quality" object or set_quality
// action parameters. Missing members preserve fallback values so older project
// files remain compatible.
inline bool TryParseQualitySettings(
    const cld::JsonValue& value,
    const QualitySettings& fallback,
    QualitySettings& settings,
    std::string& diagnostics)
{
    if (value.type != cld::JsonValue::Type::Object)
    {
        diagnostics = "quality must be an object.";
        return false;
    }

    settings = fallback;
    const QualityProfile fallbackProfile = fallback.qualityProfile;
    bool profileWasSpecified = false;
    if (const cld::JsonValue* profile = cld::FindMember(value, "qualityProfile"))
    {
        profileWasSpecified = true;
        if (profile->type != cld::JsonValue::Type::String ||
            !TryParseQualityProfile(profile->string, settings.qualityProfile))
        {
            diagnostics = "quality.qualityProfile is invalid.";
            return false;
        }
    }
    bool secondaryShadingRateWasSpecified = false;
    if (const cld::JsonValue* rate = cld::FindMember(value, "secondaryShadingRate"))
    {
        secondaryShadingRateWasSpecified = true;
        if (rate->type != cld::JsonValue::Type::String ||
            !TryParseSecondaryShadingRate(rate->string, settings.secondaryShadingRate))
        {
            diagnostics = "quality.secondaryShadingRate is invalid.";
            return false;
        }
    }

    // The automatic half-rate policy is an Interactive-only performance tool.
    // Preview and reference profiles must always shade secondary rays at full
    // rate. When a partial set_quality action changes profile, choose that
    // profile's default rather than carrying an incompatible old value across.
    if (settings.qualityProfile != QualityProfile::InteractiveGame)
    {
        settings.secondaryShadingRate = SecondaryShadingRate::Full;
    }
    else if (profileWasSpecified && settings.qualityProfile != fallbackProfile &&
        !secondaryShadingRateWasSpecified)
    {
        settings.secondaryShadingRate = DefaultSecondaryShadingRate(settings.qualityProfile);
    }
    if (const cld::JsonValue* backend = cld::FindMember(value, "restirBackend"))
    {
        if (backend->type != cld::JsonValue::Type::String ||
            !TryParseRestirBackend(backend->string, settings.restirBackend))
        {
            diagnostics = "quality.restirBackend is invalid.";
            return false;
        }
    }
    if (const cld::JsonValue* resolutionMode = cld::FindMember(value, "resolutionMode"))
    {
        if (resolutionMode->type != cld::JsonValue::Type::String ||
            !TryParseResolutionMode(resolutionMode->string, settings.resolutionMode))
        {
            diagnostics = "quality.resolutionMode is invalid.";
            return false;
        }
    }
    if (!detail::ReadFloatQualitySetting(value, "fixedRenderScale", 0.25f, 1.0f, settings.fixedRenderScale, diagnostics) ||
        !detail::ReadFloatQualitySetting(value, "minRenderScale", 0.25f, 1.0f, settings.minRenderScale, diagnostics) ||
        !detail::ReadFloatQualitySetting(value, "maxRenderScale", 0.25f, 1.0f, settings.maxRenderScale, diagnostics))
    {
        return false;
    }
    if (settings.minRenderScale > settings.maxRenderScale)
    {
        diagnostics = "quality.minRenderScale must not exceed maxRenderScale.";
        return false;
    }
    if (settings.fixedRenderScale < settings.minRenderScale ||
        settings.fixedRenderScale > settings.maxRenderScale)
    {
        diagnostics = "quality.fixedRenderScale must be within minRenderScale and maxRenderScale.";
        return false;
    }
    if (settings.qualityProfile == QualityProfile::ReferenceStill)
    {
        settings.resolutionMode = ResolutionMode::Native;
        settings.fixedRenderScale = 1.0f;
    }

    if (const cld::JsonValue* budget = cld::FindMember(value, "rayBudget"))
    {
        if (budget->type != cld::JsonValue::Type::Object)
        {
            diagnostics = "quality.rayBudget must be an object.";
            return false;
        }
        if (!detail::ReadUnsignedQualitySetting(*budget, "movingSpp", 1, 8, settings.rayBudget.movingSpp, diagnostics) ||
            !detail::ReadUnsignedQualitySetting(*budget, "movingBounces", 1, 16, settings.rayBudget.movingBounces, diagnostics) ||
            !detail::ReadUnsignedQualitySetting(*budget, "staticBaseSpp", 1, 8, settings.rayBudget.staticBaseSpp, diagnostics) ||
            !detail::ReadUnsignedQualitySetting(*budget, "staticMaxSpp", 1, 16, settings.rayBudget.staticMaxSpp, diagnostics) ||
            !detail::ReadUnsignedQualitySetting(*budget, "staticBounces", 1, 16, settings.rayBudget.staticBounces, diagnostics) ||
            !detail::ReadUnsignedQualitySetting(*budget, "settleFrames", 0, 120, settings.rayBudget.settleFrames, diagnostics))
        {
            return false;
        }

        if (const cld::JsonValue* target = cld::FindMember(*budget, "targetGpuMs"))
        {
            if (target->type != cld::JsonValue::Type::Number || !std::isfinite(target->number) ||
                target->number < 1.0 || target->number > 100.0)
            {
                diagnostics = "quality.rayBudget.targetGpuMs must be between 1 and 100.";
                return false;
            }
            settings.rayBudget.targetGpuMs = static_cast<float>(target->number);
        }
        if (settings.rayBudget.staticMaxSpp < settings.rayBudget.staticBaseSpp)
        {
            diagnostics = "quality.rayBudget.staticMaxSpp must be at least staticBaseSpp.";
            return false;
        }
    }

    if (const cld::JsonValue* finalTaa = cld::FindMember(value, "finalTaa"))
    {
        if (finalTaa->type != cld::JsonValue::Type::Bool)
        {
            diagnostics = "quality.finalTaa must be a boolean.";
            return false;
        }
        settings.finalTaa = finalTaa->boolean;
    }
    if (const cld::JsonValue* sharpen = cld::FindMember(value, "sharpenStrength"))
    {
        if (sharpen->type != cld::JsonValue::Type::Number || !std::isfinite(sharpen->number) ||
            sharpen->number < 0.0 || sharpen->number > 1.0)
        {
            diagnostics = "quality.sharpenStrength must be between 0 and 1.";
            return false;
        }
        settings.sharpenStrength = static_cast<float>(sharpen->number);
    }
    if (!detail::ReadUnsignedQualitySetting(value, "referenceSpp", 1, 1048576, settings.referenceSpp, diagnostics))
    {
        return false;
    }

    diagnostics = "Quality settings accepted.";
    return true;
}

inline std::string QualitySettingsToJson(const QualitySettings& settings)
{
    std::ostringstream json;
    json << "{\"qualityProfile\":\"" << QualityProfileName(settings.qualityProfile)
         << "\",\"restirBackend\":\"" << RestirBackendName(settings.restirBackend)
         << "\",\"secondaryShadingRate\":\"" << SecondaryShadingRateName(settings.secondaryShadingRate)
         << "\",\"resolutionMode\":\"" << ResolutionModeName(settings.resolutionMode)
         << "\",\"fixedRenderScale\":" << settings.fixedRenderScale
         << ",\"minRenderScale\":" << settings.minRenderScale
         << ",\"maxRenderScale\":" << settings.maxRenderScale
         << ",\"rayBudget\":{\"movingSpp\":" << settings.rayBudget.movingSpp
         << ",\"movingBounces\":" << settings.rayBudget.movingBounces
         << ",\"staticBaseSpp\":" << settings.rayBudget.staticBaseSpp
         << ",\"staticMaxSpp\":" << settings.rayBudget.staticMaxSpp
         << ",\"staticBounces\":" << settings.rayBudget.staticBounces
         << ",\"settleFrames\":" << settings.rayBudget.settleFrames
         << ",\"targetGpuMs\":" << settings.rayBudget.targetGpuMs
         << "},\"finalTaa\":" << (settings.finalTaa ? "true" : "false")
         << ",\"sharpenStrength\":" << settings.sharpenStrength
         << ",\"referenceSpp\":" << settings.referenceSpp << "}";
    return json.str();
}
}
