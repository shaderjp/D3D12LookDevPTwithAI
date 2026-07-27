#pragma once

#include "SimpleJson.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace rb
{
// spatialIterations belonged to the v1 denoise schema. Keep this adapter for
// one schema transition; writers should emit atrousPasses only.
inline constexpr unsigned int DenoiseSettingsSchemaVersion = 2;
inline constexpr unsigned int LegacySpatialIterationsSchemaVersion = 1;

struct DenoiseAtrousPassesCompatibility
{
    int atrousPasses = 0;
    bool specified = false;
    bool usedLegacySpatialIterations = false;
    bool ignoredLegacySpatialIterations = false;
    std::string warning;
};

namespace detail
{
inline int CompatibleDenoiseInteger(const cld::JsonValue& object, const char* name, int fallback, int minimum, int maximum)
{
    double value = cld::JsonNumberOr(object, name, static_cast<double>(fallback));
    if (!std::isfinite(value))
    {
        value = static_cast<double>(fallback);
    }
    value = std::clamp(value, static_cast<double>(minimum), static_cast<double>(maximum));
    return static_cast<int>(value);
}
}

// Resolves project and MCP denoise objects without changing their previous
// tolerant numeric behavior. Presence of atrousPasses always wins, even when
// spatialIterations is also present.
inline DenoiseAtrousPassesCompatibility ResolveDenoiseAtrousPasses(
    const cld::JsonValue& denoise,
    int fallbackAtrousPasses,
    bool allowLegacySpatialIterations = true)
{
    DenoiseAtrousPassesCompatibility result;
    result.atrousPasses = std::clamp(fallbackAtrousPasses, 0, 5);
    if (denoise.type != cld::JsonValue::Type::Object)
    {
        return result;
    }

    const cld::JsonValue* atrousPasses = cld::FindMember(denoise, "atrousPasses");
    const cld::JsonValue* spatialIterations = cld::FindMember(denoise, "spatialIterations");
    if (atrousPasses)
    {
        result.specified = true;
        result.atrousPasses = detail::CompatibleDenoiseInteger(denoise, "atrousPasses", result.atrousPasses, 0, 5);
        if (spatialIterations)
        {
            result.ignoredLegacySpatialIterations = true;
            result.warning = "denoise.spatialIterations is deprecated and was ignored because atrousPasses was also specified.";
        }
        return result;
    }

    if (spatialIterations)
    {
        result.specified = true;
        if (!allowLegacySpatialIterations)
        {
            result.ignoredLegacySpatialIterations = true;
            result.warning = "denoise.spatialIterations is only accepted when loading schema v1 projects; use atrousPasses.";
            return result;
        }
        result.usedLegacySpatialIterations = true;
        // The legacy control accepted 0..4; retain that range during the
        // direct one-to-one migration to atrousPasses.
        result.atrousPasses = detail::CompatibleDenoiseInteger(denoise, "spatialIterations", result.atrousPasses, 0, 4);
        result.warning = "denoise.spatialIterations is deprecated; its value was mapped to atrousPasses for schema v1 compatibility.";
    }
    return result;
}
}
