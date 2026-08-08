#include "QualitySettingsJson.h"
#include "SimpleJson.h"

#include <stdexcept>
#include <string>

namespace
{
void Require(bool condition, const char* message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

rb::QualitySettings Parse(
    const std::string& json,
    const rb::QualitySettings& fallback = {})
{
    const cld::JsonValue value = cld::JsonParser(json).Parse();
    rb::QualitySettings settings;
    std::string diagnostics;
    Require(rb::TryParseQualitySettings(value, fallback, settings, diagnostics), diagnostics.c_str());
    return settings;
}
}

int main()
{
    Require(rb::SecondaryShadingRateName(rb::SecondaryShadingRate::Auto) == "auto",
        "auto name mismatch");
    Require(rb::SecondaryShadingRateName(rb::SecondaryShadingRate::Full) == "full",
        "full name mismatch");
    Require(rb::SecondaryShadingRateName(rb::SecondaryShadingRate::AdaptiveHalf) == "adaptive_half",
        "adaptive_half name mismatch");

    rb::SecondaryShadingRate parsedRate = rb::SecondaryShadingRate::Full;
    Require(rb::TryParseSecondaryShadingRate("auto", parsedRate) &&
        parsedRate == rb::SecondaryShadingRate::Auto, "auto parse failed");
    Require(rb::TryParseSecondaryShadingRate("full", parsedRate) &&
        parsedRate == rb::SecondaryShadingRate::Full, "full parse failed");
    Require(rb::TryParseSecondaryShadingRate("adaptive_half", parsedRate) &&
        parsedRate == rb::SecondaryShadingRate::AdaptiveHalf, "adaptive_half parse failed");
    Require(!rb::TryParseSecondaryShadingRate("half", parsedRate), "invalid rate was accepted");

    const rb::QualitySettings oldProject = Parse("{}");
    Require(oldProject.secondaryShadingRate == rb::SecondaryShadingRate::Auto,
        "old Interactive project did not retain the auto default");
    Require(oldProject.resolutionMode == rb::ResolutionMode::Native &&
        oldProject.fixedRenderScale == 1.0f,
        "old project did not retain native resolution");
    Require(oldProject.sharpenStrength == 0.0f,
        "old project did not retain the noise-safe sharpening default");

    const rb::QualitySettings interactive = Parse(
        R"({"qualityProfile":"interactive_game","secondaryShadingRate":"adaptive_half"})");
    Require(interactive.secondaryShadingRate == rb::SecondaryShadingRate::AdaptiveHalf,
        "Interactive adaptive_half setting was not retained");

    const rb::QualitySettings sharp = Parse(
        R"({"qualityProfile":"sharp_preview","secondaryShadingRate":"adaptive_half"})");
    Require(sharp.secondaryShadingRate == rb::SecondaryShadingRate::Full,
        "Sharp Preview did not enforce full secondary shading");

    const rb::QualitySettings reference = Parse(
        R"({"qualityProfile":"reference_still","secondaryShadingRate":"auto"})");
    Require(reference.secondaryShadingRate == rb::SecondaryShadingRate::Full,
        "Reference Still did not enforce full secondary shading");

    rb::QualitySettings sharpFallback;
    sharpFallback.qualityProfile = rb::QualityProfile::SharpPreview;
    sharpFallback.secondaryShadingRate = rb::SecondaryShadingRate::Full;
    const rb::QualitySettings switchedToInteractive = Parse(
        R"({"qualityProfile":"interactive_game"})", sharpFallback);
    Require(switchedToInteractive.secondaryShadingRate == rb::SecondaryShadingRate::Auto,
        "profile switch did not select the Interactive default");

    const std::string serialized = rb::QualitySettingsToJson(interactive);
    const rb::QualitySettings roundTrip = Parse(serialized);
    Require(roundTrip.qualityProfile == interactive.qualityProfile &&
        roundTrip.secondaryShadingRate == interactive.secondaryShadingRate &&
        roundTrip.resolutionMode == interactive.resolutionMode &&
        roundTrip.fixedRenderScale == interactive.fixedRenderScale,
        "quality settings did not survive JSON round-trip");

    const rb::QualitySettings dynamic = Parse(
        R"({"resolutionMode":"dynamic","fixedRenderScale":0.75,"minRenderScale":0.5,"maxRenderScale":1.0})");
    Require(dynamic.resolutionMode == rb::ResolutionMode::Dynamic &&
        dynamic.fixedRenderScale == 0.75f && dynamic.minRenderScale == 0.5f,
        "dynamic resolution settings were not parsed");
    Require(rb::QuantizeRenderScale(0.71f) == 0.6875f,
        "render scale was not quantized to a 1/16 step");
    const rb::RenderExtent scaledExtent = rb::ResolveRenderExtent(1920u, 1080u, 0.75f);
    Require(scaledExtent.width == 1440u && scaledExtent.height == 810u &&
        scaledExtent.scale == 0.75f,
        "fixed render extent did not preserve output aspect and scale");

    bool rejectedInvalid = false;
    try
    {
        (void)Parse(R"({"secondaryShadingRate":"half"})");
    }
    catch (const std::runtime_error&)
    {
        rejectedInvalid = true;
    }
    Require(rejectedInvalid, "invalid secondary shading rate was accepted by JSON parser");
    return 0;
}
