#include "McpReviewAnalysis.h"

#include <cmath>
#include <stdexcept>
#include <string>

namespace
{
void Require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}
}

int main()
{
    using namespace lookdevpt::review;

    Bistro::Scene empty;
    const SceneAuditSummary emptyAudit = AnalyzeScene(empty);
    Require(!emptyAudit.diagnostics.empty() && emptyAudit.diagnostics.front().code == "scene.geometry.empty",
        "empty scene did not produce a stable issue code");

    AuditRuntimeState revisionA;
    revisionA.sceneRevision = 1;
    AuditRuntimeState revisionB = revisionA;
    revisionB.materialRevision = 1;
    Require(BuildAuditReport(emptyAudit, revisionA).revisionFingerprint !=
        BuildAuditReport(emptyAudit, revisionB).revisionFingerprint,
        "material revision did not invalidate the audit fingerprint");

    Bistro::Scene gltfScene;
    gltfScene.extensionsUsed = { "KHR_materials_clearcoat", "KHR_texture_basisu" };
    gltfScene.extensionsRequired = { "KHR_texture_basisu" };
    gltfScene.unsupportedExtensions = { "KHR_materials_iridescence" };
    gltfScene.materialFeatureMask = 0x61u;
    AuditRuntimeState gltfRuntime;
    gltfRuntime.textureBudgetBytes = 1024;
    gltfRuntime.textureResidentBytes = 768;
    gltfRuntime.dedicatedVideoMemoryBytes = 4096;
    const SceneAuditSummary gltfSummary = AnalyzeScene(gltfScene);
    const std::string gltfJson = BuildAuditJson(BuildAuditReport(gltfSummary, gltfRuntime));
    Require(gltfJson.find("KHR_materials_clearcoat") != std::string::npos &&
        gltfJson.find("KHR_texture_basisu") != std::string::npos &&
        gltfJson.find("KHR_materials_iridescence") != std::string::npos,
        "glTF extension audit fields are missing");
    Require(gltfJson.find("\"materialFeatureMask\":97") != std::string::npos &&
        gltfJson.find("\"budgetBytes\":1024") != std::string::npos &&
        gltfJson.find("\"residentBytes\":768") != std::string::npos &&
        gltfJson.find("\"withinBudget\":true") != std::string::npos,
        "texture residency audit fields are missing");

    Rgba8Image black{ 2, 1, { 0, 0, 0, 255, 0, 0, 0, 255 } };
    Rgba8Image same = black;
    ComparisonMetrics metrics;
    Rgba8Image heatmap;
    std::string diagnostics;
    Require(CompareImages(black, same, metrics, heatmap, diagnostics), "identical comparison failed");
    Require(metrics.linearSrgbRmse == 0.0 && metrics.luminanceSsim == 1.0 && metrics.changedPixelCount == 0,
        "identical comparison metrics are incorrect");

    Rgba8Image changed = black;
    changed.pixels[0] = 255;
    Require(CompareImages(black, changed, metrics, heatmap, diagnostics), "known-difference comparison failed");
    Require(metrics.linearSrgbRmse > 0.0 && metrics.maximumDifference == 1.0 && metrics.changedPixelCount == 1,
        "known-difference metrics are incorrect");

    Rgba8Image wrongSize{ 1, 1, { 0, 0, 0, 255 } };
    Require(!CompareImages(black, wrongSize, metrics, heatmap, diagnostics) && diagnostics == "resolution_mismatch",
        "resolution mismatch was not rejected");
    return 0;
}
