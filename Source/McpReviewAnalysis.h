#pragma once

#include "PathTracingScene.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace lookdevpt::review
{
enum class Severity
{
    Info,
    Warning,
    Error,
};

struct SceneDiagnostic
{
    std::string code;
    Severity severity = Severity::Info;
    std::string category;
    std::string entityType;
    uint32_t entityIndex = UINT32_MAX;
    std::string message;
    std::string suggestion;
};

struct SceneAuditSummary
{
    uint64_t vertexCount = 0;
    uint64_t triangleCount = 0;
    uint32_t meshCount = 0;
    uint32_t instanceCount = 0;
    uint32_t materialCount = 0;
    uint32_t textureReferenceCount = 0;
    uint32_t analyticLightCount = 0;
    uint32_t degenerateTriangleCount = 0;
    std::vector<SceneDiagnostic> diagnostics;
};

struct AuditRuntimeState
{
    uint64_t sceneRevision = 0;
    uint64_t geometryRevision = 0;
    uint64_t materialRevision = 0;
    uint64_t lightRevision = 0;
    uint64_t hdriRevision = 0;
    uint64_t backendRevision = 0;
    uint64_t profileRevision = 0;
    uint32_t activeLightCount = 0;
    bool environmentEnabled = false;
    bool environmentAvailable = false;
    bool rtxdiRequested = false;
    bool rtxdiAvailable = false;
    bool nrdRequested = false;
    bool nrdAvailable = false;
    bool dlssRequested = false;
    bool dlssAvailable = false;
};

struct AuditReport
{
    std::string revisionFingerprint;
    SceneAuditSummary summary;
    std::vector<SceneDiagnostic> diagnostics;
    uint32_t infoCount = 0;
    uint32_t warningCount = 0;
    uint32_t errorCount = 0;
};

SceneAuditSummary AnalyzeScene(const Bistro::Scene& scene);
AuditReport BuildAuditReport(const SceneAuditSummary& summary, const AuditRuntimeState& runtime);
std::string BuildAuditJson(const AuditReport& report);

struct Rgba8Image
{
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> pixels;
};

struct ComparisonMetrics
{
    double linearSrgbRmse = 0.0;
    double psnr = 0.0;
    double luminanceSsim = 1.0;
    double maximumDifference = 0.0;
    double changedPixelRatio = 0.0;
    uint64_t changedPixelCount = 0;
};

bool CompareImages(
    const Rgba8Image& before,
    const Rgba8Image& after,
    ComparisonMetrics& metrics,
    Rgba8Image& heatmap,
    std::string& diagnostics);

std::string SeverityName(Severity severity);
std::string BuildComparisonJson(
    const std::string& comparisonId,
    const std::string& beforeCaptureId,
    const std::string& afterCaptureId,
    const ComparisonMetrics& metrics,
    bool sceneFingerprintMatches,
    bool cameraFingerprintMatches);
}
