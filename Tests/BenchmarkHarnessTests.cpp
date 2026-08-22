#include "BenchmarkHarness.h"
#include "SimpleJson.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
using namespace lookdevpt::benchmark;

void Require(bool condition, const char* message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

CommandLineParseResult Parse(std::vector<std::wstring> arguments)
{
    std::vector<wchar_t*> pointers;
    pointers.reserve(arguments.size());
    for (std::wstring& argument : arguments)
    {
        pointers.push_back(argument.data());
    }
    return ParseCommandLine(static_cast<int>(pointers.size()), pointers.data());
}

MetricValues MakeMetrics(std::uint32_t frame)
{
    const double serial = static_cast<double>(frame + 1u);
    return
    {
        { "active_denoiser_nrd_reblur", 1.0 },
        { "active_restir_rtxdi_combined", 1.0 },
        { "accumulated_samples", static_cast<double>(frame + 1u) },
        { "beauty_view", 1.0 },
        { "budget_moving_bounces", 2.0 },
        { "budget_moving_spp", 1.0 },
        { "budget_settle_frames", 8.0 },
        { "budget_static_base_spp", 1.0 },
        { "budget_static_bounces", 4.0 },
        { "budget_static_max_spp", 2.0 },
        { "budget_target_gpu_ms", 14.5 },
        { "camera_motion", 0.0 },
        { "cpu_fence_wait_ms", 0.0 },
        { "cpu_frame_ms", 1.0 },
        { "final_taa_active", 1.0 },
        { "frame_context_index", static_cast<double>(frame % 3u) },
        { "frame_history_mib", 128.0 },
        { "gpu_copy_ms", 0.1 },
        { "gpu_denoise_ms", 1.0 },
        { "gpu_path_trace_ms", 8.0 },
        { "gpu_pipeline_ms", 12.0 },
        { "gpu_restir_ms", 1.0 },
        { "gpu_timing_serial", serial },
        { "gpu_timing_valid", 1.0 },
        { "gpu_ui_ms", 0.1 },
        { "history_valid", 1.0 },
        { "max_bounces", 2.0 },
        { "path_ray_budget_estimated", 4147200.0 },
        { "primary_rays_estimated", 2073600.0 },
        { "quality_profile_interactive", 1.0 },
        { "render_height", 1080.0 },
        { "render_width", 1920.0 },
        { "requested_denoiser_nrd_reblur", 1.0 },
        { "requested_restir_rtxdi_combined", 1.0 },
        { "samples_per_pixel", 1.0 },
        { "submission_serial", serial },
        { "target_adapter_rtx_4070", 1.0 },
        { "target_scene_bistro", 1.0 },
    };
}

void Write(const std::filesystem::path& path, const std::string& contents)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    Require(static_cast<bool>(file), "test file write failed");
}

std::string Read(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}
}

int wmain()
{
    try
    {
        QualityCounterTileV1 qualityTiles[2] = {};
        qualityTiles[0].taaHistoryTested = 128;
        qualityTiles[0].taaHistoryAccepted = 120;
        qualityTiles[0].disoccludedPixels = 8;
        qualityTiles[0].clampedSamples = 3;
        qualityTiles[0].contributionInputEnergy = 40.0f;
        qualityTiles[0].contributionOutputEnergy = 32.0f;
        qualityTiles[0].contributionClampedEnergy = 8.0f;
        qualityTiles[1].taaHistoryTested = 64;
        qualityTiles[1].taaHistoryAccepted = 63;
        qualityTiles[1].disoccludedPixels = 1;
        qualityTiles[1].contributionInputEnergy = 4.0f;
        qualityTiles[1].contributionOutputEnergy = 4.0f;
        MetricValues qualityMetrics;
        std::string qualityDiagnostics;
        Require(AggregateQualityCounterTiles(qualityTiles, 2, qualityMetrics, qualityDiagnostics),
            qualityDiagnostics.c_str());
        Require(qualityMetrics.at("taa_history_tested_pixels") == 192.0 &&
            qualityMetrics.at("taa_history_accepted_pixels") == 183.0 &&
            qualityMetrics.at("disoccluded_pixels") == 9.0 &&
            qualityMetrics.at("contribution_input_energy") == 44.0 &&
            qualityMetrics.at("contribution_clamped_energy") == 8.0,
            "GPU quality-counter aggregation is incorrect");
        qualityTiles[1].contributionInputEnergy = std::numeric_limits<float>::quiet_NaN();
        Require(!AggregateQualityCounterTiles(qualityTiles, 2, qualityMetrics, qualityDiagnostics),
            "non-finite GPU quality-counter energy was accepted");

        const auto legacy = Parse({ L"app.exe", L"--benchmark", L"--camera-path", L"camera.json",
            L"--frames", L"2", L"--warmup", L"1", L"--seed", L"7", L"--output", L"out" });
        Require(legacy.ok && legacy.options.benchmarkKind == BenchmarkKind::Combined &&
            legacy.options.captureEvery == 0 && !legacy.options.captureAovs,
            "legacy benchmark CLI must remain accepted");

        const auto performance = Parse({ L"app.exe", L"--benchmark", L"--benchmark-kind=performance",
            L"--camera-path=camera.json", L"--frames=2", L"--warmup=1", L"--seed=7", L"--output=out" });
        Require(performance.ok && performance.options.benchmarkKind == BenchmarkKind::Performance &&
            IncludesPerformance(performance.options.benchmarkKind) &&
            !IncludesQuality(performance.options.benchmarkKind),
            "performance-only benchmark CLI was rejected");
        const auto quality = Parse({ L"app.exe", L"--benchmark", L"--benchmark-kind", L"quality",
            L"--camera-path=camera.json", L"--frames=2", L"--warmup=1", L"--seed=7", L"--output=out" });
        Require(quality.ok && quality.options.benchmarkKind == BenchmarkKind::Quality &&
            !IncludesPerformance(quality.options.benchmarkKind) && IncludesQuality(quality.options.benchmarkKind),
            "quality-only benchmark CLI was rejected");
        Require(IncludesPerformance(BenchmarkKind::Combined) && IncludesQuality(BenchmarkKind::Combined),
            "combined benchmark must include performance and quality work");
        const auto invalidKind = Parse({ L"app.exe", L"--benchmark", L"--benchmark-kind=Performance",
            L"--camera-path=camera.json", L"--frames=2", L"--warmup=1", L"--seed=7", L"--output=out" });
        Require(!invalidKind.ok && invalidKind.diagnostics.find("combined, performance, or quality") != std::string::npos,
            "benchmark kind parsing must be exact, case-sensitive, and diagnostic");
        Require(!Parse({ L"app.exe", L"--benchmark", L"--benchmark-kind=combined", L"--benchmark-kind=quality",
            L"--camera-path=camera.json", L"--frames=2", L"--warmup=1", L"--seed=7", L"--output=out" }).ok,
            "duplicate benchmark kind was accepted");

        const auto sequence = Parse({ L"app.exe", L"--benchmark", L"--camera-path=camera.json",
            L"--frames=2", L"--warmup=1", L"--seed=7", L"--output=out",
            L"--capture-every=1", L"--capture-aovs" });
        Require(sequence.ok && sequence.options.captureEvery == 1 && sequence.options.captureAovs,
            "sequence capture CLI was rejected");
        Require(!Parse({ L"app.exe", L"--benchmark", L"--camera-path=camera.json", L"--frames=2",
            L"--warmup=1", L"--seed=7", L"--output=out", L"--capture-aovs" }).ok,
            "AOV capture without a sequence interval must be rejected");

        const auto uniqueId = std::chrono::steady_clock::now().time_since_epoch().count();
        const std::filesystem::path root = std::filesystem::temp_directory_path() /
            (L"D3D12LookDevPT-BenchmarkHarnessTests-" + std::to_wstring(uniqueId));
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root);
        const std::filesystem::path cameraPath = root / L"camera.json";
        Write(cameraPath,
            "{\"schemaVersion\":1,\"keyframes\":[{\"frame\":0,\"position\":[0,0,0],\"yaw\":0,\"pitch\":0,\"cut\":true}]}");

        Options options;
        options.enabled = true;
        options.cameraPath = cameraPath;
        options.frames = 2;
        options.warmup = 1;
        options.seed = 7;
        options.outputDirectory = root / L"output";
        options.captureEvery = 1;
        options.benchmarkKind = BenchmarkKind::Performance;
        Harness harness(options);
        std::string diagnostics;
        Require(harness.Initialize(diagnostics), diagnostics.c_str());
        Require(!harness.ShouldCaptureFrame(0) && harness.ShouldCaptureFrame(1) && harness.ShouldCaptureFrame(2),
            "capture schedule is incorrect");

        for (std::uint32_t frame = 0; frame < harness.TotalFrames(); ++frame)
        {
            Require(harness.RecordFrameMetrics(frame, MakeMetrics(frame), diagnostics), diagnostics.c_str());
        }

        Write(options.outputDirectory / L"frames/000000/beauty_hdr.hdr", "abc");
        Write(options.outputDirectory / L"frames/000001/beauty_hdr.hdr", "def");
        ArtifactRecord artifact;
        artifact.relativePath = L"frames/000000/beauty_hdr.hdr";
        artifact.role = "beauty_hdr";
        artifact.encoding = "radiance_hdr";
        artifact.sourceFormat = "R16G16B16A16_FLOAT";
        artifact.width = 1920;
        artifact.height = 1080;
        artifact.phase = ArtifactPhase::MeasuredFrame;
        artifact.frameIndex = 1;
        artifact.measuredFrameIndex = 0;
        artifact.statistics.available = true;
        artifact.statistics.channelCount = 3;
        artifact.statistics.channelMin = { 0.0, 0.0, 0.0, 0.0 };
        artifact.statistics.channelMax = { 1.0, 1.0, 1.0, 0.0 };
        Require(harness.RegisterArtifact(artifact, diagnostics), diagnostics.c_str());
        artifact.relativePath = L"frames/000001/beauty_hdr.hdr";
        artifact.frameIndex = 2;
        artifact.measuredFrameIndex = 1;
        Require(harness.RegisterArtifact(artifact, diagnostics), diagnostics.c_str());

        ArtifactRecord unsafe = artifact;
        unsafe.relativePath = L"../escape.hdr";
        Require(!harness.RegisterArtifact(unsafe, diagnostics), "path traversal artifact was accepted");
        Require(harness.WriteOutputs(diagnostics), diagnostics.c_str());

        const std::string manifest = Read(options.outputDirectory / L"artifacts.json");
        Require(cld::JsonParser(manifest).Parse().type == cld::JsonValue::Type::Object,
            "artifact manifest is not valid JSON");
        Require(manifest.find("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") != std::string::npos,
            "SHA-256 manifest digest is incorrect");
        Require(manifest.find("\"allFinite\": true") != std::string::npos,
            "finite artifact statistics did not pass validation");
        const std::string analysis = Read(options.outputDirectory / L"quality_analysis.json");
        Require(cld::JsonParser(analysis).Parse().type == cld::JsonValue::Type::Object,
            "quality analysis contract is not valid JSON");
        Require(analysis.find("\"ready\": true") != std::string::npos,
            "contiguous HDR sequence was not marked ready for temporal CV");
        const std::string summary = Read(options.outputDirectory / L"summary.json");
        Require(cld::JsonParser(summary).Parse().type == cld::JsonValue::Type::Object,
            "benchmark summary is not valid JSON");
        Require(summary.find("\"benchmarkKind\": \"performance\"") != std::string::npos &&
            summary.find("\"isolatedPerformanceRun\": true") != std::string::npos,
            "benchmark kind was not published to the summary or performance gate");

        std::filesystem::remove_all(root);
        std::cout << "BenchmarkHarnessTests passed\n";
        return 0;
    }
    catch (const std::exception& exception)
    {
        std::cerr << "BenchmarkHarnessTests failed: " << exception.what() << '\n';
        return 1;
    }
}
