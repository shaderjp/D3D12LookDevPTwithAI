#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <map>
#include <string>
#include <vector>

namespace lookdevpt::benchmark
{
inline constexpr double FixedDeltaSeconds = 1.0 / 60.0;
inline constexpr std::uint32_t InvalidMeasuredFrameIndex = std::numeric_limits<std::uint32_t>::max();

enum class BenchmarkKind : std::uint8_t
{
    Combined,
    Performance,
    Quality,
};

constexpr bool IncludesPerformance(BenchmarkKind kind)
{
    return kind == BenchmarkKind::Combined || kind == BenchmarkKind::Performance;
}

constexpr bool IncludesQuality(BenchmarkKind kind)
{
    return kind == BenchmarkKind::Combined || kind == BenchmarkKind::Quality;
}

const char* BenchmarkKindName(BenchmarkKind kind);

struct Options
{
    bool enabled = false;
    BenchmarkKind benchmarkKind = BenchmarkKind::Combined;
    std::filesystem::path cameraPath;
    std::uint32_t frames = 0;
    std::uint32_t warmup = 0;
    std::uint64_t seed = 0;
    std::filesystem::path outputDirectory;
    // Zero preserves the historical final-artifact-only behavior. A positive
    // value captures measured frames whose measuredFrameIndex is divisible by
    // this interval. Sequence captures always include LDR and HDR beauty.
    std::uint32_t captureEvery = 0;
    bool captureAovs = false;
};

struct CommandLineParseResult
{
    bool ok = false;
    Options options;
    std::string diagnostics;
};

// Arguments include argv[0]. With rejectUnknownArguments=true this accepts
// only the benchmark CLI. Integration with the application's existing CLI can
// pass false while benchmark options remain strictly validated.
CommandLineParseResult ParseCommandLine(
    int argc,
    wchar_t* const argv[],
    bool rejectUnknownArguments = true);

struct CameraKeyframe
{
    std::uint32_t frame = 0;
    std::array<float, 3> position = {};
    float yaw = 0.0f;
    float pitch = 0.0f;
    bool cut = false;
};

struct CameraSample
{
    std::array<float, 3> position = {};
    float yaw = 0.0f;
    float pitch = 0.0f;
    bool cut = false;
};

class CameraPath
{
public:
    // JSON shape:
    // {"schemaVersion":1,"keyframes":[
    //   {"frame":0,"position":[0,1,2],"yaw":0,"pitch":0,"cut":true}
    // ]}
    bool Load(const std::filesystem::path& path, std::string& diagnostics);
    bool Sample(std::uint32_t frame, CameraSample& sample, std::string& diagnostics) const;

    const std::vector<CameraKeyframe>& Keyframes() const { return m_keyframes; }

private:
    std::vector<CameraKeyframe> m_keyframes;
};

struct FramePlan
{
    std::uint32_t frameIndex = 0;
    std::uint32_t measuredFrameIndex = InvalidMeasuredFrameIndex;
    bool warmup = true;
    double deltaSeconds = FixedDeltaSeconds;
    double elapsedSeconds = 0.0;
    CameraSample camera;
};

using MetricValues = std::map<std::string, double>;

struct FrameMetrics
{
    std::uint32_t frameIndex = 0;
    MetricValues values;
};

enum class ArtifactPhase : std::uint8_t
{
    Final,
    MeasuredFrame,
};

struct ArtifactStatistics
{
    bool available = false;
    std::uint32_t channelCount = 0;
    std::uint64_t nonFiniteValueCount = 0;
    std::uint64_t nonFinitePixelCount = 0;
    std::array<double, 4> channelMin = {};
    std::array<double, 4> channelMax = {};
};

struct ArtifactRecord
{
    // Paths are relative to Options::outputDirectory. Absolute paths and '..'
    // are rejected so manifests remain relocatable and cannot escape the run.
    std::filesystem::path relativePath;
    std::string role;
    std::string encoding;
    std::string sourceFormat;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    ArtifactPhase phase = ArtifactPhase::Final;
    std::uint32_t frameIndex = InvalidMeasuredFrameIndex;
    std::uint32_t measuredFrameIndex = InvalidMeasuredFrameIndex;
    ArtifactStatistics statistics;
};

// Forward-compatible GPU quality-counter ABI. The renderer should allocate
// one 64-byte record per dispatch tile and let exactly one thread group write
// each record after a group-local reduction. This avoids global floating-point
// atomics and 32-bit fixed-point energy overflow. The CPU may then aggregate
// records in double precision and publish the metric names described in the
// benchmark manifest. The D3D12 backend allocates these records only while the
// deterministic benchmark harness is active.
inline constexpr std::uint32_t QualityCounterAbiVersion = 2;
struct alignas(16) QualityCounterTileV1
{
    std::uint32_t surfaceHistoryTested = 0;
    std::uint32_t surfaceHistoryAccepted = 0;
    std::uint32_t rejectOutOfBounds = 0;
    std::uint32_t rejectDepth = 0;

    std::uint32_t rejectNormal = 0;
    std::uint32_t rejectRoughness = 0;
    std::uint32_t rejectIdentity = 0;
    std::uint32_t acceptedByDilation = 0;

    std::uint32_t taaHistoryTested = 0;
    std::uint32_t taaHistoryAccepted = 0;
    std::uint32_t disoccludedPixels = 0;
    std::uint32_t clampedSamples = 0;

    float contributionInputEnergy = 0.0f;
    float contributionOutputEnergy = 0.0f;
    float contributionClampedEnergy = 0.0f;
    std::uint32_t nonFinitePixels = 0;

    std::uint32_t primaryRays = 0;
    std::uint32_t secondaryRays = 0;
    std::uint32_t shadowRays = 0;
    std::uint32_t diVisibilityRays = 0;

    std::uint32_t giVisibilityRays = 0;
    std::uint32_t ptVisibilityRays = 0;
    std::uint32_t anyHitInvocations = 0;
    std::uint32_t reserved = 0;
};
static_assert(sizeof(QualityCounterTileV1) == 96);
static_assert(alignof(QualityCounterTileV1) == 16);
static_assert(offsetof(QualityCounterTileV1, surfaceHistoryTested) == 0);
static_assert(offsetof(QualityCounterTileV1, taaHistoryTested) == 32);
static_assert(offsetof(QualityCounterTileV1, contributionInputEnergy) == 48);
static_assert(offsetof(QualityCounterTileV1, nonFinitePixels) == 60);
static_assert(offsetof(QualityCounterTileV1, primaryRays) == 64);
static_assert(offsetof(QualityCounterTileV1, anyHitInvocations) == 88);

// Reduces GPU-written tile records in double precision and appends the
// quality-counter metric contract to `metrics`. Invalid/non-finite records are
// rejected instead of being published as available benchmark data.
bool AggregateQualityCounterTiles(
    const QualityCounterTileV1* tiles,
    std::size_t tileCount,
    MetricValues& metrics,
    std::string& diagnostics);

class Harness
{
public:
    explicit Harness(Options options);

    bool Initialize(std::string& diagnostics);
    bool GetFramePlan(std::uint32_t frameIndex, FramePlan& plan, std::string& diagnostics) const;
    bool RecordFrameMetrics(std::uint32_t frameIndex, const MetricValues& values, std::string& diagnostics);
    bool RegisterArtifact(const ArtifactRecord& artifact, std::string& diagnostics);
    bool WriteOutputs(std::string& diagnostics) const;

    bool ShouldCaptureFrame(std::uint32_t frameIndex) const;

    std::uint32_t TotalFrames() const;
    const Options& GetOptions() const { return m_options; }
    const CameraPath& GetCameraPath() const { return m_cameraPath; }
    const std::vector<FrameMetrics>& Metrics() const { return m_metrics; }
    const std::vector<ArtifactRecord>& Artifacts() const { return m_artifacts; }

private:
    Options m_options;
    CameraPath m_cameraPath;
    std::vector<FrameMetrics> m_metrics;
    std::vector<std::string> m_metricNames;
    std::vector<ArtifactRecord> m_artifacts;
    bool m_initialized = false;
};
}
