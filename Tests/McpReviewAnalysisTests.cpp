#include "McpReviewAnalysis.h"

#include <cmath>
#include <stdexcept>

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
