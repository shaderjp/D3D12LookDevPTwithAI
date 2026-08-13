#include "McpReviewAnalysis.h"

#include "SimpleJson.h"

#include <DirectXMath.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iomanip>
#include <limits>
#include <numeric>
#include <sstream>
#include <unordered_map>

namespace lookdevpt::review
{
namespace
{
bool IsFinite(float value)
{
    return std::isfinite(value);
}

bool IsFinite(const DirectX::XMFLOAT2& value)
{
    return IsFinite(value.x) && IsFinite(value.y);
}

bool IsFinite(const DirectX::XMFLOAT3& value)
{
    return IsFinite(value.x) && IsFinite(value.y) && IsFinite(value.z);
}

bool IsFinite(const DirectX::XMFLOAT4& value)
{
    return IsFinite(value.x) && IsFinite(value.y) && IsFinite(value.z) && IsFinite(value.w);
}

void AddDiagnostic(
    SceneAuditSummary& summary,
    std::string code,
    Severity severity,
    std::string category,
    std::string entityType,
    uint32_t entityIndex,
    std::string message,
    std::string suggestion)
{
    summary.diagnostics.push_back(SceneDiagnostic{
        std::move(code), severity, std::move(category), std::move(entityType), entityIndex,
        std::move(message), std::move(suggestion) });
}

double SrgbToLinear(uint8_t channel)
{
    const double value = static_cast<double>(channel) / 255.0;
    return value <= 0.04045 ? value / 12.92 : std::pow((value + 0.055) / 1.055, 2.4);
}

std::string EntityJson(const SceneDiagnostic& diagnostic)
{
    std::ostringstream json;
    json << "{\"type\":\"" << cld::EscapeJson(diagnostic.entityType) << "\"";
    if (diagnostic.entityIndex != UINT32_MAX)
    {
        json << ",\"index\":" << diagnostic.entityIndex;
    }
    json << "}";
    return json.str();
}
}

std::string SeverityName(Severity severity)
{
    switch (severity)
    {
    case Severity::Error: return "error";
    case Severity::Warning: return "warning";
    default: return "info";
    }
}

SceneAuditSummary AnalyzeScene(const Bistro::Scene& scene)
{
    SceneAuditSummary result;
    result.vertexCount = scene.vertices.size();
    result.triangleCount = scene.indices.size() / 3u;
    result.meshCount = static_cast<uint32_t>(scene.meshes.size());
    result.instanceCount = static_cast<uint32_t>(scene.instances.size());
    result.materialCount = static_cast<uint32_t>(scene.materials.size());
    result.analyticLightCount = static_cast<uint32_t>(scene.analyticLights.size());

    if (scene.vertices.empty() || scene.indices.empty())
    {
        AddDiagnostic(result, "scene.geometry.empty", Severity::Error, "geometry", "scene", UINT32_MAX,
            "The scene has no renderable triangle geometry.", "Load a scene containing at least one indexed triangle mesh.");
    }
    if ((scene.indices.size() % 3u) != 0u)
    {
        AddDiagnostic(result, "geometry.index_count.not_triangles", Severity::Error, "geometry", "scene", UINT32_MAX,
            "The index count is not divisible by three.", "Repair the source mesh topology before importing it.");
    }

    for (uint32_t i = 0; i < scene.vertices.size(); ++i)
    {
        const auto& vertex = scene.vertices[i];
        if (!IsFinite(vertex.position) || !IsFinite(vertex.normal) ||
            !IsFinite(vertex.tangent) || !IsFinite(vertex.texcoord))
        {
            AddDiagnostic(result, "geometry.vertex.non_finite", Severity::Error, "geometry", "vertex", i,
                "A vertex contains NaN or infinity.", "Re-export the mesh after repairing invalid vertex attributes.");
        }
    }

    for (uint32_t triangle = 0; triangle < scene.indices.size() / 3u; ++triangle)
    {
        const uint32_t i0 = scene.indices[triangle * 3u + 0u];
        const uint32_t i1 = scene.indices[triangle * 3u + 1u];
        const uint32_t i2 = scene.indices[triangle * 3u + 2u];
        if (i0 >= scene.vertices.size() || i1 >= scene.vertices.size() || i2 >= scene.vertices.size())
        {
            AddDiagnostic(result, "geometry.index.out_of_range", Severity::Error, "geometry", "primitive", triangle,
                "A triangle references a vertex outside the vertex buffer.", "Repair the mesh index buffer.");
            continue;
        }
        const DirectX::XMFLOAT3& a = scene.vertices[i0].position;
        const DirectX::XMFLOAT3& b = scene.vertices[i1].position;
        const DirectX::XMFLOAT3& c = scene.vertices[i2].position;
        const double abx = static_cast<double>(b.x) - a.x;
        const double aby = static_cast<double>(b.y) - a.y;
        const double abz = static_cast<double>(b.z) - a.z;
        const double acx = static_cast<double>(c.x) - a.x;
        const double acy = static_cast<double>(c.y) - a.y;
        const double acz = static_cast<double>(c.z) - a.z;
        const double cx = aby * acz - abz * acy;
        const double cy = abz * acx - abx * acz;
        const double cz = abx * acy - aby * acx;
        if (cx * cx + cy * cy + cz * cz <= 1.0e-20)
        {
            ++result.degenerateTriangleCount;
        }
    }
    if (result.degenerateTriangleCount != 0)
    {
        AddDiagnostic(result, "geometry.primitive.degenerate", Severity::Warning, "geometry", "scene", UINT32_MAX,
            std::to_string(result.degenerateTriangleCount) + " degenerate triangles were found.",
            "Remove zero-area faces or weld duplicate vertices in the source asset.");
    }

    std::vector<uint32_t> materialUse(scene.materials.size(), 0u);
    for (uint32_t i = 0; i < scene.draws.size(); ++i)
    {
        const auto& draw = scene.draws[i];
        if (draw.materialIndex >= scene.materials.size())
        {
            AddDiagnostic(result, "material.reference.out_of_range", Severity::Error, "material", "draw", i,
                "A draw references a missing material.", "Assign a valid material to the source primitive.");
        }
        else
        {
            ++materialUse[draw.materialIndex];
        }
        if (static_cast<uint64_t>(draw.startIndex) + draw.indexCount > scene.indices.size())
        {
            AddDiagnostic(result, "geometry.draw.out_of_range", Severity::Error, "geometry", "draw", i,
                "A draw range extends beyond the index buffer.", "Repair the imported mesh range metadata.");
        }
    }

    std::unordered_map<std::wstring, uint32_t> materialNames;
    for (uint32_t i = 0; i < scene.materials.size(); ++i)
    {
        const auto& material = scene.materials[i];
        if (!material.name.empty())
        {
            const auto [position, inserted] = materialNames.emplace(material.name, i);
            if (!inserted)
            {
                AddDiagnostic(result, "material.name.duplicate", Severity::Warning, "material", "material", i,
                    "Multiple materials use the same name.", "Give materials unique names for unambiguous MCP targeting.");
            }
        }
        if (i < materialUse.size() && materialUse[i] == 0u)
        {
            AddDiagnostic(result, "material.unused", Severity::Info, "material", "material", i,
                "The material is not referenced by any draw.", "Remove it or assign it to geometry if it is intentional.");
        }
        for (uint32_t slot = 0; slot < material.textures.size(); ++slot)
        {
            if (material.textures[slot].empty())
            {
                continue;
            }
            ++result.textureReferenceCount;
            std::filesystem::path path(material.textures[slot]);
            if (path.is_relative())
            {
                path = std::filesystem::path(scene.assetRoot) / path;
            }
            std::error_code error;
            if (!std::filesystem::exists(path, error))
            {
                AddDiagnostic(result, "texture.file.missing", Severity::Warning, "texture", "material", i,
                    "A referenced texture file is missing.", "Relink the texture or remove the stale material slot.");
            }
        }
    }

    for (uint32_t i = 0; i < scene.instances.size(); ++i)
    {
        if (scene.instances[i].meshIndex >= scene.meshes.size())
        {
            AddDiagnostic(result, "geometry.instance.mesh_missing", Severity::Error, "geometry", "instance", i,
                "An instance references a missing mesh.", "Repair the scene instance hierarchy.");
        }
        const auto& matrix = scene.instances[i].transform;
        if (!IsFinite(matrix._11) || !IsFinite(matrix._12) || !IsFinite(matrix._13) || !IsFinite(matrix._14) ||
            !IsFinite(matrix._21) || !IsFinite(matrix._22) || !IsFinite(matrix._23) || !IsFinite(matrix._24) ||
            !IsFinite(matrix._31) || !IsFinite(matrix._32) || !IsFinite(matrix._33) || !IsFinite(matrix._34) ||
            !IsFinite(matrix._41) || !IsFinite(matrix._42) || !IsFinite(matrix._43) || !IsFinite(matrix._44))
        {
            AddDiagnostic(result, "geometry.instance.transform_non_finite", Severity::Error, "geometry", "instance", i,
                "An instance transform contains NaN or infinity.", "Reset or repair the source node transform.");
        }
    }
    return result;
}

AuditReport BuildAuditReport(const SceneAuditSummary& summary, const AuditRuntimeState& runtime)
{
    AuditReport report;
    report.summary = summary;
    report.diagnostics = summary.diagnostics;
    std::ostringstream fingerprint;
    fingerprint << std::hex << runtime.sceneRevision << '-' << runtime.geometryRevision << '-'
        << runtime.materialRevision << '-' << runtime.lightRevision << '-' << runtime.hdriRevision << '-'
        << runtime.backendRevision << '-' << runtime.profileRevision;
    report.revisionFingerprint = fingerprint.str();

    auto addRuntime = [&](const char* code, Severity severity, const char* category,
        const char* message, const char* suggestion)
    {
        report.diagnostics.push_back(SceneDiagnostic{
            code, severity, category, "renderer", UINT32_MAX, message, suggestion });
    };
    if (runtime.activeLightCount == 0u && !(runtime.environmentEnabled && runtime.environmentAvailable))
    {
        addRuntime("lighting.no_effective_source", Severity::Warning, "lighting",
            "No analytic, emissive, or available environment light is active.",
            "Enable an HDRI or add a light with non-zero radiance.");
    }
    if (runtime.environmentEnabled && !runtime.environmentAvailable)
    {
        addRuntime("lighting.hdri.unavailable", Severity::Warning, "lighting",
            "Environment lighting is enabled but the HDRI is unavailable.", "Load a valid HDR or EXR environment map.");
    }
    if (runtime.rtxdiRequested && !runtime.rtxdiAvailable)
    {
        addRuntime("backend.rtxdi.fallback", Severity::Info, "backend",
            "RTXDI was requested but the renderer is using its fallback path.", "Install or enable the RTXDI backend to use it.");
    }
    if (runtime.nrdRequested && !runtime.nrdAvailable)
    {
        addRuntime("backend.nrd.fallback", Severity::Info, "backend",
            "NRD was requested but the internal denoiser is active.", "Install or enable NRD, or select the fallback explicitly.");
    }
    if (runtime.dlssRequested && !runtime.dlssAvailable)
    {
        addRuntime("backend.dlss.fallback", Severity::Info, "backend",
            "DLSS was requested but the renderer is using native or fallback reconstruction.",
            "Use supported hardware and install the DLSS runtime, or select native reconstruction.");
    }

    for (const auto& diagnostic : report.diagnostics)
    {
        switch (diagnostic.severity)
        {
        case Severity::Error: ++report.errorCount; break;
        case Severity::Warning: ++report.warningCount; break;
        default: ++report.infoCount; break;
        }
    }
    return report;
}

std::string BuildAuditJson(const AuditReport& report)
{
    std::ostringstream json;
    json << "{\"revisionFingerprint\":\"" << cld::EscapeJson(report.revisionFingerprint)
        << "\",\"counts\":{\"error\":" << report.errorCount << ",\"warning\":" << report.warningCount
        << ",\"info\":" << report.infoCount << "},\"scene\":{\"vertices\":" << report.summary.vertexCount
        << ",\"triangles\":" << report.summary.triangleCount << ",\"meshes\":" << report.summary.meshCount
        << ",\"instances\":" << report.summary.instanceCount << ",\"materials\":" << report.summary.materialCount
        << ",\"textureReferences\":" << report.summary.textureReferenceCount << ",\"analyticLights\":"
        << report.summary.analyticLightCount << ",\"degenerateTriangles\":" << report.summary.degenerateTriangleCount
        << "},\"issues\":[";
    for (size_t i = 0; i < report.diagnostics.size(); ++i)
    {
        if (i != 0) json << ',';
        const auto& item = report.diagnostics[i];
        json << "{\"code\":\"" << cld::EscapeJson(item.code) << "\",\"severity\":\""
            << SeverityName(item.severity) << "\",\"category\":\"" << cld::EscapeJson(item.category)
            << "\",\"entity\":" << EntityJson(item) << ",\"message\":\"" << cld::EscapeJson(item.message)
            << "\",\"suggestion\":\"" << cld::EscapeJson(item.suggestion) << "\"}";
    }
    json << "]}";
    return json.str();
}

bool CompareImages(
    const Rgba8Image& before,
    const Rgba8Image& after,
    ComparisonMetrics& metrics,
    Rgba8Image& heatmap,
    std::string& diagnostics)
{
    metrics = {};
    heatmap = {};
    diagnostics.clear();
    if (before.width == 0 || before.height == 0 || before.width != after.width || before.height != after.height)
    {
        diagnostics = "resolution_mismatch";
        return false;
    }
    const uint64_t pixelCount = static_cast<uint64_t>(before.width) * before.height;
    if (before.pixels.size() != pixelCount * 4u || after.pixels.size() != pixelCount * 4u)
    {
        diagnostics = "invalid_rgba8_image";
        return false;
    }

    heatmap.width = before.width;
    heatmap.height = before.height;
    heatmap.pixels.resize(static_cast<size_t>(pixelCount) * 4u);
    double squaredError = 0.0;
    double maximumDifference = 0.0;
    double sumBeforeLuminance = 0.0;
    double sumAfterLuminance = 0.0;
    std::vector<double> beforeLuminance(static_cast<size_t>(pixelCount));
    std::vector<double> afterLuminance(static_cast<size_t>(pixelCount));

    for (uint64_t pixel = 0; pixel < pixelCount; ++pixel)
    {
        double maximumPixelDifference = 0.0;
        double beforeRgb[3] = {};
        double afterRgb[3] = {};
        for (uint32_t channel = 0; channel < 3u; ++channel)
        {
            beforeRgb[channel] = SrgbToLinear(before.pixels[pixel * 4u + channel]);
            afterRgb[channel] = SrgbToLinear(after.pixels[pixel * 4u + channel]);
            const double difference = std::abs(beforeRgb[channel] - afterRgb[channel]);
            squaredError += difference * difference;
            maximumDifference = std::max(maximumDifference, difference);
            maximumPixelDifference = std::max(maximumPixelDifference, difference);
        }
        if (maximumPixelDifference > (1.0 / 255.0))
        {
            ++metrics.changedPixelCount;
        }
        const double value = std::clamp(maximumPixelDifference * 4.0, 0.0, 1.0);
        const double red = std::clamp(value * 3.0, 0.0, 1.0);
        const double green = std::clamp(3.0 - std::abs(value * 6.0 - 3.0), 0.0, 1.0);
        const double blue = std::clamp(3.0 - value * 3.0, 0.0, 1.0);
        heatmap.pixels[pixel * 4u + 0u] = static_cast<uint8_t>(std::round(red * 255.0));
        heatmap.pixels[pixel * 4u + 1u] = static_cast<uint8_t>(std::round(green * 255.0));
        heatmap.pixels[pixel * 4u + 2u] = static_cast<uint8_t>(std::round(blue * 255.0));
        heatmap.pixels[pixel * 4u + 3u] = 255u;

        const double luminanceBefore = beforeRgb[0] * 0.2126 + beforeRgb[1] * 0.7152 + beforeRgb[2] * 0.0722;
        const double luminanceAfter = afterRgb[0] * 0.2126 + afterRgb[1] * 0.7152 + afterRgb[2] * 0.0722;
        beforeLuminance[pixel] = luminanceBefore;
        afterLuminance[pixel] = luminanceAfter;
        sumBeforeLuminance += luminanceBefore;
        sumAfterLuminance += luminanceAfter;
    }

    metrics.linearSrgbRmse = std::sqrt(squaredError / static_cast<double>(pixelCount * 3u));
    metrics.psnr = metrics.linearSrgbRmse == 0.0
        ? std::numeric_limits<double>::infinity()
        : 20.0 * std::log10(1.0 / metrics.linearSrgbRmse);
    metrics.maximumDifference = maximumDifference;
    metrics.changedPixelRatio = static_cast<double>(metrics.changedPixelCount) / static_cast<double>(pixelCount);

    const double meanBefore = sumBeforeLuminance / static_cast<double>(pixelCount);
    const double meanAfter = sumAfterLuminance / static_cast<double>(pixelCount);
    double varianceBefore = 0.0;
    double varianceAfter = 0.0;
    double covariance = 0.0;
    for (uint64_t pixel = 0; pixel < pixelCount; ++pixel)
    {
        const double beforeDelta = beforeLuminance[pixel] - meanBefore;
        const double afterDelta = afterLuminance[pixel] - meanAfter;
        varianceBefore += beforeDelta * beforeDelta;
        varianceAfter += afterDelta * afterDelta;
        covariance += beforeDelta * afterDelta;
    }
    const double divisor = static_cast<double>(std::max<uint64_t>(pixelCount - 1u, 1u));
    varianceBefore /= divisor;
    varianceAfter /= divisor;
    covariance /= divisor;
    constexpr double c1 = 0.01 * 0.01;
    constexpr double c2 = 0.03 * 0.03;
    metrics.luminanceSsim = ((2.0 * meanBefore * meanAfter + c1) * (2.0 * covariance + c2)) /
        ((meanBefore * meanBefore + meanAfter * meanAfter + c1) * (varianceBefore + varianceAfter + c2));
    diagnostics.clear();
    return true;
}

std::string BuildComparisonJson(
    const std::string& comparisonId,
    const std::string& beforeCaptureId,
    const std::string& afterCaptureId,
    const ComparisonMetrics& metrics,
    bool sceneFingerprintMatches,
    bool cameraFingerprintMatches)
{
    std::ostringstream json;
    json << std::setprecision(10) << "{\"id\":\"" << cld::EscapeJson(comparisonId)
        << "\",\"beforeCaptureId\":\"" << cld::EscapeJson(beforeCaptureId)
        << "\",\"afterCaptureId\":\"" << cld::EscapeJson(afterCaptureId)
        << "\",\"metrics\":{\"linearSrgbRmse\":" << metrics.linearSrgbRmse << ",\"psnr\":";
    if (std::isinf(metrics.psnr)) json << "null"; else json << metrics.psnr;
    json << ",\"luminanceSsim\":" << metrics.luminanceSsim << ",\"maximumDifference\":"
        << metrics.maximumDifference << ",\"changedPixelRatio\":" << metrics.changedPixelRatio
        << ",\"changedPixelCount\":" << metrics.changedPixelCount << "},\"fingerprints\":{\"sceneMatches\":"
        << (sceneFingerprintMatches ? "true" : "false") << ",\"cameraMatches\":"
        << (cameraFingerprintMatches ? "true" : "false") << "},\"heatmapUri\":\"lookdevpt://comparisons/"
        << cld::EscapeJson(comparisonId) << "/heatmap.png\"}";
    return json.str();
}
}
