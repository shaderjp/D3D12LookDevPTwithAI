#include "stdafx.h"
#include "BenchmarkHarness.h"

#include "SimpleJson.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <initializer_list>
#include <limits>
#include <locale>
#include <set>
#include <sstream>
#include <system_error>
#include <utility>

namespace lookdevpt::benchmark
{
namespace
{
constexpr std::uint64_t MaximumFrameCount = std::numeric_limits<std::uint32_t>::max();
constexpr std::uintmax_t MaximumCameraPathBytes = 16u * 1024u * 1024u;
constexpr double Pi = 3.14159265358979323846;

constexpr std::array<std::uint32_t, 64> Sha256RoundConstants =
{
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};

class Sha256
{
public:
    void Update(const std::uint8_t* bytes, std::size_t size)
    {
        if (!bytes || size == 0)
        {
            return;
        }
        m_totalBytes += size;
        while (size > 0)
        {
            const std::size_t amount = (std::min)(size, m_block.size() - m_blockBytes);
            std::memcpy(m_block.data() + m_blockBytes, bytes, amount);
            m_blockBytes += amount;
            bytes += amount;
            size -= amount;
            if (m_blockBytes == m_block.size())
            {
                Transform(m_block.data());
                m_blockBytes = 0;
            }
        }
    }

    std::array<std::uint8_t, 32> Finalize()
    {
        const std::uint64_t bitCount = m_totalBytes * 8u;
        m_block[m_blockBytes++] = 0x80u;
        if (m_blockBytes > 56)
        {
            std::fill(m_block.begin() + m_blockBytes, m_block.end(), std::uint8_t{});
            Transform(m_block.data());
            m_blockBytes = 0;
        }
        std::fill(m_block.begin() + m_blockBytes, m_block.begin() + 56, std::uint8_t{});
        for (std::size_t byte = 0; byte < 8; ++byte)
        {
            m_block[63 - byte] = static_cast<std::uint8_t>(bitCount >> (byte * 8));
        }
        Transform(m_block.data());

        std::array<std::uint8_t, 32> digest = {};
        for (std::size_t word = 0; word < m_state.size(); ++word)
        {
            digest[word * 4 + 0] = static_cast<std::uint8_t>(m_state[word] >> 24);
            digest[word * 4 + 1] = static_cast<std::uint8_t>(m_state[word] >> 16);
            digest[word * 4 + 2] = static_cast<std::uint8_t>(m_state[word] >> 8);
            digest[word * 4 + 3] = static_cast<std::uint8_t>(m_state[word]);
        }
        return digest;
    }

private:
    static std::uint32_t ReadBigEndian(const std::uint8_t* bytes)
    {
        return (static_cast<std::uint32_t>(bytes[0]) << 24) |
            (static_cast<std::uint32_t>(bytes[1]) << 16) |
            (static_cast<std::uint32_t>(bytes[2]) << 8) |
            static_cast<std::uint32_t>(bytes[3]);
    }

    void Transform(const std::uint8_t* block)
    {
        std::array<std::uint32_t, 64> schedule = {};
        for (std::size_t word = 0; word < 16; ++word)
        {
            schedule[word] = ReadBigEndian(block + word * 4);
        }
        for (std::size_t word = 16; word < schedule.size(); ++word)
        {
            const std::uint32_t x = schedule[word - 15];
            const std::uint32_t y = schedule[word - 2];
            const std::uint32_t sigma0 = std::rotr(x, 7) ^ std::rotr(x, 18) ^ (x >> 3);
            const std::uint32_t sigma1 = std::rotr(y, 17) ^ std::rotr(y, 19) ^ (y >> 10);
            schedule[word] = schedule[word - 16] + sigma0 + schedule[word - 7] + sigma1;
        }

        std::uint32_t a = m_state[0];
        std::uint32_t b = m_state[1];
        std::uint32_t c = m_state[2];
        std::uint32_t d = m_state[3];
        std::uint32_t e = m_state[4];
        std::uint32_t f = m_state[5];
        std::uint32_t g = m_state[6];
        std::uint32_t h = m_state[7];
        for (std::size_t round = 0; round < schedule.size(); ++round)
        {
            const std::uint32_t sum1 = std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25);
            const std::uint32_t choice = (e & f) ^ (~e & g);
            const std::uint32_t temporary1 = h + sum1 + choice + Sha256RoundConstants[round] + schedule[round];
            const std::uint32_t sum0 = std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22);
            const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
            const std::uint32_t temporary2 = sum0 + majority;
            h = g;
            g = f;
            f = e;
            e = d + temporary1;
            d = c;
            c = b;
            b = a;
            a = temporary1 + temporary2;
        }
        m_state[0] += a;
        m_state[1] += b;
        m_state[2] += c;
        m_state[3] += d;
        m_state[4] += e;
        m_state[5] += f;
        m_state[6] += g;
        m_state[7] += h;
    }

    std::array<std::uint32_t, 8> m_state =
    {
        0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
        0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u,
    };
    std::array<std::uint8_t, 64> m_block = {};
    std::uint64_t m_totalBytes = 0;
    std::size_t m_blockBytes = 0;
};

struct ParsedArgument
{
    std::wstring name;
    std::wstring inlineValue;
    bool hasInlineValue = false;
};

ParsedArgument SplitArgument(const std::wstring& argument)
{
    ParsedArgument parsed;
    const std::size_t equals = argument.find(L'=');
    if (equals == std::wstring::npos)
    {
        parsed.name = argument;
        return parsed;
    }
    parsed.name = argument.substr(0, equals);
    parsed.inlineValue = argument.substr(equals + 1);
    parsed.hasInlineValue = true;
    return parsed;
}

bool IsBenchmarkValueArgument(const std::wstring& name)
{
    return name == L"--camera-path" || name == L"--frames" || name == L"--warmup" ||
        name == L"--seed" || name == L"--output" || name == L"--capture-every" ||
        name == L"--benchmark-kind";
}

bool ParseBenchmarkKind(const std::wstring& text, BenchmarkKind& kind)
{
    if (text == L"combined")
    {
        kind = BenchmarkKind::Combined;
        return true;
    }
    if (text == L"performance")
    {
        kind = BenchmarkKind::Performance;
        return true;
    }
    if (text == L"quality")
    {
        kind = BenchmarkKind::Quality;
        return true;
    }
    return false;
}

bool ParseUnsigned(const std::wstring& text, std::uint64_t maximum, std::uint64_t& value)
{
    if (text.empty())
    {
        return false;
    }
    std::uint64_t parsed = 0;
    for (const wchar_t ch : text)
    {
        if (ch < L'0' || ch > L'9')
        {
            return false;
        }
        const std::uint64_t digit = static_cast<std::uint64_t>(ch - L'0');
        if (parsed > (maximum - digit) / 10)
        {
            return false;
        }
        parsed = parsed * 10 + digit;
    }
    value = parsed;
    return true;
}

std::string PathToUtf8(const std::filesystem::path& path)
{
    const std::u8string utf8 = path.generic_u8string();
    return std::string(reinterpret_cast<const char*>(utf8.data()), utf8.size());
}

bool IsExactInteger(const cld::JsonValue& value, std::uint32_t& result)
{
    if (value.type != cld::JsonValue::Type::Number || !std::isfinite(value.number) ||
        value.number < 0.0 || value.number > static_cast<double>(std::numeric_limits<std::uint32_t>::max()) ||
        std::floor(value.number) != value.number)
    {
        return false;
    }
    result = static_cast<std::uint32_t>(value.number);
    return true;
}

bool ReadFiniteFloat(const cld::JsonValue& value, float& result)
{
    constexpr double FloatMax = static_cast<double>(std::numeric_limits<float>::max());
    if (value.type != cld::JsonValue::Type::Number || !std::isfinite(value.number) ||
        value.number < -FloatMax || value.number > FloatMax)
    {
        return false;
    }
    result = static_cast<float>(value.number);
    return true;
}

bool HasOnlyMembers(const cld::JsonValue& object, std::initializer_list<const char*> allowed, std::string& unknown)
{
    std::set<std::string> names;
    for (const char* name : allowed)
    {
        names.emplace(name);
    }
    for (const auto& [name, value] : object.object)
    {
        (void)value;
        if (!names.contains(name))
        {
            unknown = name;
            return false;
        }
    }
    return true;
}

bool IsValidMetricName(const std::string& name)
{
    if (name.empty() || name.size() > 96)
    {
        return false;
    }
    const auto isAlpha = [](char ch)
    {
        return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '_';
    };
    const auto isTail = [&](char ch)
    {
        return isAlpha(ch) || (ch >= '0' && ch <= '9') || ch == '.' || ch == '-';
    };
    if (!isAlpha(name.front()) || !std::all_of(name.begin() + 1, name.end(), isTail))
    {
        return false;
    }
    return name != "frame_index" && name != "measured_frame" && name != "phase" && name != "fixed_delta_seconds";
}

std::string EscapeCsv(const std::string& value)
{
    if (value.find_first_of(",\"\r\n") == std::string::npos)
    {
        return value;
    }
    std::string escaped = "\"";
    for (const char ch : value)
    {
        escaped.push_back(ch);
        if (ch == '"')
        {
            escaped.push_back('"');
        }
    }
    escaped.push_back('"');
    return escaped;
}

double Percentile(const std::vector<double>& sortedValues, double percentile)
{
    if (sortedValues.empty())
    {
        return 0.0;
    }
    const double position = percentile * static_cast<double>(sortedValues.size() - 1);
    const std::size_t lower = static_cast<std::size_t>(std::floor(position));
    const std::size_t upper = static_cast<std::size_t>(std::ceil(position));
    const double amount = position - static_cast<double>(lower);
    return sortedValues[lower] + (sortedValues[upper] - sortedValues[lower]) * amount;
}

bool WriteTextFile(const std::filesystem::path& path, const std::string& text, std::string& diagnostics)
{
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file)
    {
        diagnostics = "Could not open benchmark output file: " + PathToUtf8(path);
        return false;
    }
    file.write(text.data(), static_cast<std::streamsize>(text.size()));
    file.close();
    if (!file)
    {
        diagnostics = "Could not write benchmark output file: " + PathToUtf8(path);
        return false;
    }
    return true;
}

bool HashFileSha256(
    const std::filesystem::path& path,
    std::string& hexadecimalDigest,
    std::uintmax_t& fileBytes,
    std::string& diagnostics)
{
    std::error_code sizeError;
    fileBytes = std::filesystem::file_size(path, sizeError);
    if (sizeError)
    {
        diagnostics = "Could not stat benchmark artifact: " + PathToUtf8(path);
        return false;
    }

    std::ifstream file(path, std::ios::binary);
    if (!file)
    {
        diagnostics = "Could not read benchmark artifact: " + PathToUtf8(path);
        return false;
    }

    Sha256 sha256;
    std::array<std::uint8_t, 64u * 1024u> buffer = {};
    for (;;)
    {
        file.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize bytesRead = file.gcount();
        if (bytesRead > 0)
        {
            sha256.Update(buffer.data(), static_cast<std::size_t>(bytesRead));
        }
        if (file.eof())
        {
            break;
        }
        if (!file)
        {
            diagnostics = "Could not hash benchmark artifact: " + PathToUtf8(path);
            return false;
        }
    }

    const std::array<std::uint8_t, 32> digest = sha256.Finalize();
    std::ostringstream hash;
    hash << std::hex << std::setfill('0');
    for (const std::uint8_t byte : digest)
    {
        hash << std::setw(2) << static_cast<unsigned>(byte);
    }
    hexadecimalDigest = hash.str();
    return true;
}

bool IsSafeArtifactPath(const std::filesystem::path& path)
{
    if (path.empty() || path.is_absolute() || path.has_root_name() || path.has_root_directory())
    {
        return false;
    }
    for (const std::filesystem::path& component : path)
    {
        if (component == L"..")
        {
            return false;
        }
    }
    return true;
}

bool IsArtifactToken(const std::string& text)
{
    if (text.empty() || text.size() > 96)
    {
        return false;
    }
    return std::all_of(text.begin(), text.end(), [](const char ch)
    {
        return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
            (ch >= '0' && ch <= '9') || ch == '_' || ch == '-' || ch == '.';
    });
}

struct MetricDefinition
{
    const char* unit = "scalar";
    const char* source = "renderer";
    const char* description = "Renderer-provided benchmark metric.";
};

const MetricDefinition& DescribeMetric(const std::string& name)
{
    static const std::map<std::string, MetricDefinition> definitions =
    {
        { "cpu_frame_ms", { "ms", "cpu", "CPU active frame work excluding GPU fence throttling." } },
        { "cpu_fence_wait_ms", { "ms", "cpu", "CPU time blocked waiting for an in-flight frame context." } },
        { "cpu_update_ms", { "ms", "cpu", "Camera, renderer-state, and constant-buffer update work." } },
        { "cpu_mcp_ms", { "ms", "cpu", "MCP command processing and snapshot publication work." } },
        { "cpu_ui_ms", { "ms", "cpu", "Editor command processing and immutable snapshot publication." } },
        { "cpu_command_recording_ms", { "ms", "cpu", "D3D12 command-list recording excluding frame-context waits." } },
        { "cpu_present_ms", { "ms", "cpu", "CPU time spent in swap-chain Present." } },
        { "cpu_nrd_recording_ms", { "ms", "cpu", "NRD descriptor preparation and dispatch recording work." } },
        { "cpu_benchmark_aggregate_ms", { "ms", "cpu", "Per-frame benchmark metric snapshot construction work." } },
        { "gpu_pipeline_ms", { "ms", "gpu_timestamp", "End-to-end timed GPU rendering pipeline." } },
        { "gpu_path_trace_ms", { "ms", "gpu_timestamp", "DXR path tracing segment." } },
        { "gpu_restir_ms", { "ms", "gpu_timestamp", "ReSTIR resampling and resolve segment." } },
        { "gpu_restir_candidate_ms", { "ms", "gpu_timestamp", "ReSTIR candidate generation segment." } },
        { "gpu_restir_temporal_ms", { "ms", "gpu_timestamp", "ReSTIR temporal resampling segment." } },
        { "gpu_restir_spatial_ms", { "ms", "gpu_timestamp", "ReSTIR spatial resampling segment." } },
        { "gpu_restir_shade_ms", { "ms", "gpu_timestamp", "ReSTIR visibility and shading segment." } },
        { "gpu_restir_publish_ms", { "ms", "gpu_timestamp", "ReSTIR history publication segment." } },
        { "gpu_restir_gi_initial_ms", { "ms", "gpu_timestamp", "ReSTIR GI secondary-path initial sampling segment." } },
        { "gpu_restir_gi_fused_ms", { "ms", "gpu_timestamp", "ReSTIR GI temporal/spatial reuse, final visibility, and shading segment." } },
        { "gpu_restir_pt_initial_ms", { "ms", "gpu_timestamp", "ReSTIR PT initial path sampling segment." } },
        { "gpu_restir_pt_fused_ms", { "ms", "gpu_timestamp", "ReSTIR PT temporal/spatial shift and final shading segment." } },
        { "gpu_denoise_ms", { "ms", "gpu_timestamp", "Denoising and final temporal resolve segment." } },
        { "gpu_denoise_prepare_ms", { "ms", "gpu_timestamp", "Denoiser input preparation or fallback temporal stage." } },
        { "gpu_denoise_core_ms", { "ms", "gpu_timestamp", "NRD dispatch graph or fallback spatial filtering stage." } },
        { "gpu_denoise_composite_ms", { "ms", "gpu_timestamp", "Denoised signal composite stage." } },
        { "gpu_final_taa_ms", { "ms", "gpu_timestamp", "Final HDR temporal resolve and sharpen stage." } },
        { "gpu_quality_counters_ms", { "ms", "gpu_timestamp", "Full-screen diagnostic quality-counter stage." } },
        { "gpu_history_publish_ms", { "ms", "gpu_timestamp", "Surface-guide and identity history publication stage." } },
        { "gpu_copy_ms", { "ms", "gpu_timestamp", "Output copy segment." } },
        { "gpu_ui_ms", { "ms", "gpu_timestamp", "Final swap-chain transition segment; WinUI is composition-owned and has no renderer UI draw." } },
        { "gpu_timing_valid", { "boolean", "gpu_timestamp", "One when all GPU timestamps for this submission are valid." } },
        { "submission_serial", { "serial", "renderer", "Monotonic command-queue submission serial." } },
        { "gpu_timing_serial", { "serial", "gpu_timestamp", "Submission serial associated with delayed GPU timestamp readback." } },
        { "frame_context_index", { "index", "renderer", "Triple-buffered frame-context index." } },
        { "primary_rays_estimated", { "rays", "estimate", "Estimated primary rays from resolution and configured SPP." } },
        { "path_ray_budget_estimated", { "rays", "estimate", "Upper-bound path-ray estimate from primary rays and bounce budget." } },
        { "samples_per_pixel", { "samples/pixel", "renderer", "Active sample count per pixel." } },
        { "max_bounces", { "bounces", "renderer", "Active maximum path depth." } },
        { "diagnostic_counters_enabled", { "boolean", "renderer", "One when full-screen quality diagnostics execute for this run." } },
        { "camera_motion", { "scalar", "renderer", "Normalized camera-motion amount used by quality control." } },
        { "history_valid", { "boolean", "renderer", "Coarse denoiser-history validity; not a per-pixel acceptance rate." } },
        { "accumulated_samples", { "samples", "renderer", "Progressive accumulation sample count." } },
        { "frame_history_mib", { "MiB", "resource_accounting", "Frame and history resource allocation, excluding documented backend pools." } },
        { "vram_frame_history_peak_mib", { "MiB", "resource_accounting", "Peak resident frame/history allocation after lifetime aliasing." } },
        { "vram_restir_alias_heap_mib", { "MiB", "resource_accounting", "Resident placed-resource heaps shared by DI scratch and GI history." } },
        { "compacted_blas_bytes", { "bytes", "resource_accounting", "Resident bottom-level acceleration structure size after compaction." } },
        { "blas_compaction_ratio", { "ratio", "resource_accounting", "Compacted BLAS bytes divided by original build-result bytes." } },
        { "render_width", { "pixels", "renderer", "Native render width." } },
        { "render_height", { "pixels", "renderer", "Native render height." } },
        { "output_width", { "pixels", "renderer", "Display/output width." } },
        { "output_height", { "pixels", "renderer", "Display/output height." } },
        { "render_scale", { "ratio", "renderer", "Internal render scale quantized to 1/16 steps." } },
        { "dynamic_resolution_active", { "boolean", "renderer", "One while the dynamic-resolution controller is selected." } },
        { "taau_active", { "boolean", "renderer", "One when Final TAA temporally upsamples to display resolution." } },
        { "primary_visibility_separate", { "boolean", "renderer", "One when interactive primary visibility runs as an independent DXR pass." } },
        { "compact_secondary_worklist", { "boolean", "renderer", "One when per-pixel SPP requirements are prefix-summed into a compact task list." } },
        { "secondary_execute_indirect", { "boolean", "renderer", "One when the compact 1D DXR workload uses ExecuteIndirect on DXR 1.1." } },
        { "secondary_task_capacity", { "tasks", "resource_accounting", "Allocated compact secondary task/result capacity." } },
        { "requested_dlss_rr", { "boolean", "renderer", "One when DLSS Ray Reconstruction is selected and enabled." } },
        { "dlss_runtime_available", { "boolean", "streamline", "One when the Streamline interposer was loaded." } },
        { "dlss_initialized", { "boolean", "streamline", "One after successful Streamline initialization." } },
        { "dlss_device_registered", { "boolean", "streamline", "One after the D3D12 device is registered." } },
        { "dlss_application_identity_configured", { "boolean", "streamline", "One when a NVIDIA-issued NGX application ID is configured through the process environment." } },
        { "dlss_feature_supported", { "boolean", "streamline", "One when DLSS Ray Reconstruction is supported by the adapter and driver." } },
        { "dlss_evaluation_ready", { "boolean", "streamline", "One while the complete DLSS-RR evaluation contract is ready." } },
        { "active_dlss_rr", { "boolean", "streamline", "One only after DLSS-RR evaluated successfully for the frame." } },
        { "dlss_successful_evaluations", { "evaluations", "streamline", "Cumulative successful DLSS-RR evaluations." } },
        { "dlss_failed_evaluations", { "evaluations", "streamline", "Cumulative failed DLSS-RR evaluations." } },
        { "dlss_last_result_code", { "enum", "streamline", "Numeric Streamline result from the latest setup or evaluation operation." } },
        { "surface_history_tested_pixels", { "pixels", "gpu_quality_counter", "Surface pixels offered to validated reprojection." } },
        { "surface_history_accepted_pixels", { "pixels", "gpu_quality_counter", "Surface pixels with accepted reprojection." } },
        { "surface_history_reject_oob_pixels", { "pixels", "gpu_quality_counter", "Surface history rejected outside the previous viewport." } },
        { "surface_history_reject_depth_pixels", { "pixels", "gpu_quality_counter", "Surface history rejected by linear view-Z." } },
        { "surface_history_reject_normal_pixels", { "pixels", "gpu_quality_counter", "Surface history rejected by normal." } },
        { "surface_history_reject_roughness_pixels", { "pixels", "gpu_quality_counter", "Surface history rejected by roughness." } },
        { "surface_history_reject_identity_pixels", { "pixels", "gpu_quality_counter", "Surface history rejected by packed identity or coverage." } },
        { "surface_history_dilated_pixels", { "pixels", "gpu_quality_counter", "Surface history accepted only by neighborhood dilation." } },
        { "taa_history_tested_pixels", { "pixels", "gpu_quality_counter", "Pixels tested against final HDR TAA history." } },
        { "taa_history_accepted_pixels", { "pixels", "gpu_quality_counter", "Pixels accepting final HDR TAA history." } },
        { "disoccluded_pixels", { "pixels", "gpu_quality_counter", "Pixels with invalid or newly exposed history." } },
        { "contribution_input_energy", { "linear-radiance-sum", "gpu_quality_counter", "Luminance energy before interactive contribution compression." } },
        { "contribution_output_energy", { "linear-radiance-sum", "gpu_quality_counter", "Luminance energy after interactive contribution compression." } },
        { "contribution_clamped_energy", { "linear-radiance-sum", "gpu_quality_counter", "Luminance energy removed by contribution compression." } },
        { "contribution_clamped_samples", { "samples", "gpu_quality_counter", "Pixel estimators modified by contribution compression." } },
        { "non_finite_pixels", { "pixels", "gpu_quality_counter", "Validation pixels containing NaN or infinity." } },
        { "primary_rays_actual", { "rays", "gpu_shader_counter", "Primary TraceRay invocations measured by the path-tracing shaders." } },
        { "secondary_rays_actual", { "rays", "gpu_shader_counter", "Secondary path TraceRay or inline-RayQuery invocations." } },
        { "shadow_rays_actual", { "rays", "gpu_shader_counter", "Baseline next-event visibility TraceRay invocations." } },
        { "di_visibility_rays_actual", { "rays", "gpu_shader_counter", "RTXDI DI final visibility RayQuery invocations." } },
        { "gi_visibility_rays_actual", { "rays", "gpu_shader_counter", "RTXDI GI secondary-light and final visibility RayQuery invocations." } },
        { "pt_visibility_rays_actual", { "rays", "gpu_shader_counter", "RTXDI PT path-light and final reconnection visibility RayQuery invocations." } },
        { "anyhit_invocations_actual", { "invocations", "gpu_shader_counter", "DXR AnyHit shader invocations; this is not a BVH node-traversal count." } },
    };
    static const MetricDefinition fallback;
    const auto found = definitions.find(name);
    return found == definitions.end() ? fallback : found->second;
}

struct QualityMetricContract
{
    const char* name;
    const char* unit;
    const char* description;
};

constexpr std::array<QualityMetricContract, 23> QualityMetricContracts =
{{
    { "surface_history_tested_pixels", "pixels", "Surface pixels offered to validated reprojection." },
    { "surface_history_accepted_pixels", "pixels", "Surface pixels with at least one accepted reprojection tap." },
    { "surface_history_reject_oob_pixels", "pixels", "Rejected because every tap was outside the previous viewport." },
    { "surface_history_reject_depth_pixels", "pixels", "Rejected by linear view-Z validation." },
    { "surface_history_reject_normal_pixels", "pixels", "Rejected by normal validation." },
    { "surface_history_reject_roughness_pixels", "pixels", "Rejected by roughness validation." },
    { "surface_history_reject_identity_pixels", "pixels", "Rejected by material, instance, or primitive identity." },
    { "surface_history_dilated_pixels", "pixels", "Accepted only by depth-aware neighborhood dilation." },
    { "taa_history_tested_pixels", "pixels", "Pixels offered to final HDR TAA history." },
    { "taa_history_accepted_pixels", "pixels", "Pixels that accepted final HDR TAA history." },
    { "disoccluded_pixels", "pixels", "Pixels classified as newly visible or invalid history." },
    { "contribution_input_energy", "linear-radiance-sum", "Signal energy before contribution compression or clamping." },
    { "contribution_output_energy", "linear-radiance-sum", "Signal energy after contribution compression or clamping." },
    { "contribution_clamped_energy", "linear-radiance-sum", "Energy removed by contribution compression or clamping." },
    { "contribution_clamped_samples", "samples", "Contributions modified by compression or clamping." },
    { "non_finite_pixels", "pixels", "Pixels containing NaN or infinity at validation output." },
    { "primary_rays_actual", "rays", "Measured primary path TraceRay invocations." },
    { "secondary_rays_actual", "rays", "Measured secondary path ray invocations." },
    { "shadow_rays_actual", "rays", "Measured Baseline visibility TraceRay invocations." },
    { "di_visibility_rays_actual", "rays", "Measured RTXDI DI final visibility rays." },
    { "gi_visibility_rays_actual", "rays", "Measured RTXDI GI visibility rays." },
    { "pt_visibility_rays_actual", "rays", "Measured RTXDI PT visibility rays." },
    { "anyhit_invocations_actual", "invocations", "Measured AnyHit shader invocations, not BVH traversal nodes." },
}};
}

const char* BenchmarkKindName(BenchmarkKind kind)
{
    switch (kind)
    {
    case BenchmarkKind::Combined: return "combined";
    case BenchmarkKind::Performance: return "performance";
    case BenchmarkKind::Quality: return "quality";
    }
    return "invalid";
}

bool AggregateQualityCounterTiles(
    const QualityCounterTileV1* tiles,
    std::size_t tileCount,
    MetricValues& metrics,
    std::string& diagnostics)
{
    if (!tiles || tileCount == 0)
    {
        diagnostics = "GPU quality-counter readback is empty.";
        return false;
    }

    std::uint64_t surfaceHistoryTested = 0;
    std::uint64_t surfaceHistoryAccepted = 0;
    std::uint64_t rejectOutOfBounds = 0;
    std::uint64_t rejectDepth = 0;
    std::uint64_t rejectNormal = 0;
    std::uint64_t rejectRoughness = 0;
    std::uint64_t rejectIdentity = 0;
    std::uint64_t acceptedByDilation = 0;
    std::uint64_t taaHistoryTested = 0;
    std::uint64_t taaHistoryAccepted = 0;
    std::uint64_t disoccludedPixels = 0;
    std::uint64_t clampedSamples = 0;
    std::uint64_t nonFinitePixels = 0;
    std::uint64_t primaryRays = 0;
    std::uint64_t secondaryRays = 0;
    std::uint64_t shadowRays = 0;
    std::uint64_t diVisibilityRays = 0;
    std::uint64_t giVisibilityRays = 0;
    std::uint64_t ptVisibilityRays = 0;
    std::uint64_t anyHitInvocations = 0;
    double contributionInputEnergy = 0.0;
    double contributionOutputEnergy = 0.0;
    double contributionClampedEnergy = 0.0;

    for (std::size_t tileIndex = 0; tileIndex < tileCount; ++tileIndex)
    {
        const QualityCounterTileV1& tile = tiles[tileIndex];
        if (!std::isfinite(tile.contributionInputEnergy) || tile.contributionInputEnergy < 0.0f ||
            !std::isfinite(tile.contributionOutputEnergy) || tile.contributionOutputEnergy < 0.0f ||
            !std::isfinite(tile.contributionClampedEnergy) || tile.contributionClampedEnergy < 0.0f)
        {
            diagnostics = "GPU quality-counter tile contains invalid energy.";
            return false;
        }
        surfaceHistoryTested += tile.surfaceHistoryTested;
        surfaceHistoryAccepted += tile.surfaceHistoryAccepted;
        rejectOutOfBounds += tile.rejectOutOfBounds;
        rejectDepth += tile.rejectDepth;
        rejectNormal += tile.rejectNormal;
        rejectRoughness += tile.rejectRoughness;
        rejectIdentity += tile.rejectIdentity;
        acceptedByDilation += tile.acceptedByDilation;
        taaHistoryTested += tile.taaHistoryTested;
        taaHistoryAccepted += tile.taaHistoryAccepted;
        disoccludedPixels += tile.disoccludedPixels;
        clampedSamples += tile.clampedSamples;
        nonFinitePixels += tile.nonFinitePixels;
        primaryRays += tile.primaryRays;
        secondaryRays += tile.secondaryRays;
        shadowRays += tile.shadowRays;
        diVisibilityRays += tile.diVisibilityRays;
        giVisibilityRays += tile.giVisibilityRays;
        ptVisibilityRays += tile.ptVisibilityRays;
        anyHitInvocations += tile.anyHitInvocations;
        contributionInputEnergy += static_cast<double>(tile.contributionInputEnergy);
        contributionOutputEnergy += static_cast<double>(tile.contributionOutputEnergy);
        contributionClampedEnergy += static_cast<double>(tile.contributionClampedEnergy);
    }

    metrics["surface_history_tested_pixels"] = static_cast<double>(surfaceHistoryTested);
    metrics["surface_history_accepted_pixels"] = static_cast<double>(surfaceHistoryAccepted);
    metrics["surface_history_reject_oob_pixels"] = static_cast<double>(rejectOutOfBounds);
    metrics["surface_history_reject_depth_pixels"] = static_cast<double>(rejectDepth);
    metrics["surface_history_reject_normal_pixels"] = static_cast<double>(rejectNormal);
    metrics["surface_history_reject_roughness_pixels"] = static_cast<double>(rejectRoughness);
    metrics["surface_history_reject_identity_pixels"] = static_cast<double>(rejectIdentity);
    metrics["surface_history_dilated_pixels"] = static_cast<double>(acceptedByDilation);
    metrics["taa_history_tested_pixels"] = static_cast<double>(taaHistoryTested);
    metrics["taa_history_accepted_pixels"] = static_cast<double>(taaHistoryAccepted);
    metrics["disoccluded_pixels"] = static_cast<double>(disoccludedPixels);
    metrics["contribution_input_energy"] = contributionInputEnergy;
    metrics["contribution_output_energy"] = contributionOutputEnergy;
    metrics["contribution_clamped_energy"] = contributionClampedEnergy;
    metrics["contribution_clamped_samples"] = static_cast<double>(clampedSamples);
    metrics["non_finite_pixels"] = static_cast<double>(nonFinitePixels);
    metrics["primary_rays_actual"] = static_cast<double>(primaryRays);
    metrics["secondary_rays_actual"] = static_cast<double>(secondaryRays);
    metrics["shadow_rays_actual"] = static_cast<double>(shadowRays);
    metrics["di_visibility_rays_actual"] = static_cast<double>(diVisibilityRays);
    metrics["gi_visibility_rays_actual"] = static_cast<double>(giVisibilityRays);
    metrics["pt_visibility_rays_actual"] = static_cast<double>(ptVisibilityRays);
    metrics["anyhit_invocations_actual"] = static_cast<double>(anyHitInvocations);
    diagnostics = "GPU quality-counter tiles aggregated.";
    return true;
}

CommandLineParseResult ParseCommandLine(int argc, wchar_t* const argv[], bool rejectUnknownArguments)
{
    CommandLineParseResult result;
    if (argc < 0 || (argc > 0 && argv == nullptr))
    {
        result.diagnostics = "Benchmark command line is invalid.";
        return result;
    }

    bool benchmarkSeen = false;
    bool benchmarkValueSeen = false;
    bool cameraPathSeen = false;
    bool framesSeen = false;
    bool warmupSeen = false;
    bool seedSeen = false;
    bool outputSeen = false;
    bool captureEverySeen = false;
    bool captureAovsSeen = false;
    bool benchmarkKindSeen = false;

    auto consumeValue = [&](int& index, const ParsedArgument& argument, std::wstring& value) -> bool
    {
        if (argument.hasInlineValue)
        {
            value = argument.inlineValue;
        }
        else if (index + 1 < argc)
        {
            value = argv[++index] ? argv[index] : L"";
        }
        if (value.empty())
        {
            result.diagnostics = "Benchmark option requires a non-empty value.";
            return false;
        }
        return true;
    };

    for (int index = 1; index < argc; ++index)
    {
        if (!argv[index])
        {
            result.diagnostics = "Benchmark command line contains a null argument.";
            return result;
        }
        const ParsedArgument argument = SplitArgument(argv[index]);
        if (argument.name == L"--benchmark")
        {
            if (argument.hasInlineValue)
            {
                result.diagnostics = "--benchmark does not accept a value.";
                return result;
            }
            if (benchmarkSeen)
            {
                result.diagnostics = "--benchmark was specified more than once.";
                return result;
            }
            benchmarkSeen = true;
            continue;
        }

        if (argument.name == L"--capture-aovs")
        {
            benchmarkValueSeen = true;
            if (argument.hasInlineValue)
            {
                result.diagnostics = "--capture-aovs does not accept a value.";
                return result;
            }
            if (captureAovsSeen)
            {
                result.diagnostics = "--capture-aovs was specified more than once.";
                return result;
            }
            captureAovsSeen = true;
            result.options.captureAovs = true;
            continue;
        }

        if (!IsBenchmarkValueArgument(argument.name))
        {
            if (rejectUnknownArguments)
            {
                result.diagnostics = "Unknown benchmark command-line argument.";
                return result;
            }
            continue;
        }

        benchmarkValueSeen = true;
        std::wstring value;
        if (!consumeValue(index, argument, value))
        {
            return result;
        }

        bool* seen = nullptr;
        if (argument.name == L"--camera-path") seen = &cameraPathSeen;
        else if (argument.name == L"--frames") seen = &framesSeen;
        else if (argument.name == L"--warmup") seen = &warmupSeen;
        else if (argument.name == L"--seed") seen = &seedSeen;
        else if (argument.name == L"--output") seen = &outputSeen;
        else if (argument.name == L"--capture-every") seen = &captureEverySeen;
        else if (argument.name == L"--benchmark-kind") seen = &benchmarkKindSeen;
        if (*seen)
        {
            result.diagnostics = "A benchmark option was specified more than once.";
            return result;
        }
        *seen = true;

        if (argument.name == L"--camera-path")
        {
            result.options.cameraPath = std::filesystem::path(value);
        }
        else if (argument.name == L"--output")
        {
            result.options.outputDirectory = std::filesystem::path(value);
        }
        else if (argument.name == L"--benchmark-kind")
        {
            if (!ParseBenchmarkKind(value, result.options.benchmarkKind))
            {
                result.diagnostics = "--benchmark-kind must be exactly combined, performance, or quality.";
                return result;
            }
        }
        else
        {
            std::uint64_t parsed = 0;
            const std::uint64_t maximum = argument.name == L"--seed"
                ? std::numeric_limits<std::uint64_t>::max()
                : MaximumFrameCount;
            if (!ParseUnsigned(value, maximum, parsed))
            {
                result.diagnostics = "Benchmark numeric options require unsigned decimal integers in range.";
                return result;
            }
            if (argument.name == L"--frames") result.options.frames = static_cast<std::uint32_t>(parsed);
            else if (argument.name == L"--warmup") result.options.warmup = static_cast<std::uint32_t>(parsed);
            else if (argument.name == L"--capture-every") result.options.captureEvery = static_cast<std::uint32_t>(parsed);
            else result.options.seed = parsed;
        }
    }

    if (!benchmarkSeen)
    {
        if (benchmarkValueSeen)
        {
            result.diagnostics = "Benchmark options require --benchmark.";
            return result;
        }
        result.ok = true;
        result.diagnostics = "Benchmark mode is not requested.";
        return result;
    }
    if (!cameraPathSeen || !framesSeen || !warmupSeen || !seedSeen || !outputSeen)
    {
        result.diagnostics = "--benchmark requires --camera-path, --frames, --warmup, --seed, and --output.";
        return result;
    }
    if (result.options.frames == 0)
    {
        result.diagnostics = "--frames must be greater than zero.";
        return result;
    }
    if (captureEverySeen && result.options.captureEvery == 0)
    {
        result.diagnostics = "--capture-every must be greater than zero.";
        return result;
    }
    if (result.options.captureAovs && result.options.captureEvery == 0)
    {
        result.diagnostics = "--capture-aovs requires --capture-every.";
        return result;
    }
    if (static_cast<std::uint64_t>(result.options.frames) + result.options.warmup > MaximumFrameCount)
    {
        result.diagnostics = "Warmup and measured frame counts overflow the supported range.";
        return result;
    }

    result.options.enabled = true;
    result.ok = true;
    result.diagnostics = "Benchmark command line accepted.";
    return result;
}

bool CameraPath::Load(const std::filesystem::path& path, std::string& diagnostics)
{
    m_keyframes.clear();
    std::error_code sizeError;
    const std::uintmax_t fileSize = std::filesystem::file_size(path, sizeError);
    if (sizeError)
    {
        diagnostics = "Camera path file was not found: " + PathToUtf8(path);
        return false;
    }
    if (fileSize == 0 || fileSize > MaximumCameraPathBytes)
    {
        diagnostics = "Camera path file must be between 1 byte and 16 MiB.";
        return false;
    }

    std::ifstream file(path, std::ios::binary);
    std::string text(static_cast<std::size_t>(fileSize), '\0');
    file.read(text.data(), static_cast<std::streamsize>(text.size()));
    if (!file)
    {
        diagnostics = "Could not read camera path file.";
        return false;
    }
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xef &&
        static_cast<unsigned char>(text[1]) == 0xbb && static_cast<unsigned char>(text[2]) == 0xbf)
    {
        text.erase(0, 3);
    }

    try
    {
        const cld::JsonValue root = cld::JsonParser(text).Parse();
        if (root.type != cld::JsonValue::Type::Object)
        {
            diagnostics = "Camera path root must be an object.";
            return false;
        }
        std::string unknown;
        if (!HasOnlyMembers(root, { "schemaVersion", "keyframes" }, unknown))
        {
            diagnostics = "Unknown camera path member: " + unknown;
            return false;
        }
        if (const cld::JsonValue* version = cld::FindMember(root, "schemaVersion"))
        {
            std::uint32_t parsedVersion = 0;
            if (!IsExactInteger(*version, parsedVersion) || parsedVersion != 1)
            {
                diagnostics = "Camera path schemaVersion must be 1.";
                return false;
            }
        }

        const cld::JsonValue* keyframes = cld::FindMember(root, "keyframes");
        if (!keyframes || keyframes->type != cld::JsonValue::Type::Array || keyframes->array.empty())
        {
            diagnostics = "Camera path requires a non-empty keyframes array.";
            return false;
        }
        if (keyframes->array.size() > 1000000)
        {
            diagnostics = "Camera path contains too many keyframes.";
            return false;
        }

        m_keyframes.reserve(keyframes->array.size());
        for (std::size_t index = 0; index < keyframes->array.size(); ++index)
        {
            const cld::JsonValue& value = keyframes->array[index];
            if (value.type != cld::JsonValue::Type::Object)
            {
                diagnostics = "Each camera keyframe must be an object.";
                m_keyframes.clear();
                return false;
            }
            if (!HasOnlyMembers(value, { "frame", "position", "yaw", "pitch", "cut" }, unknown))
            {
                diagnostics = "Unknown camera keyframe member: " + unknown;
                m_keyframes.clear();
                return false;
            }

            const cld::JsonValue* frame = cld::FindMember(value, "frame");
            const cld::JsonValue* position = cld::FindMember(value, "position");
            const cld::JsonValue* yaw = cld::FindMember(value, "yaw");
            const cld::JsonValue* pitch = cld::FindMember(value, "pitch");
            if (!frame || !position || !yaw || !pitch || position->type != cld::JsonValue::Type::Array || position->array.size() != 3)
            {
                diagnostics = "Camera keyframes require frame, position[3], yaw, and pitch.";
                m_keyframes.clear();
                return false;
            }

            CameraKeyframe keyframe;
            if (!IsExactInteger(*frame, keyframe.frame) ||
                !ReadFiniteFloat(position->array[0], keyframe.position[0]) ||
                !ReadFiniteFloat(position->array[1], keyframe.position[1]) ||
                !ReadFiniteFloat(position->array[2], keyframe.position[2]) ||
                !ReadFiniteFloat(*yaw, keyframe.yaw) || !ReadFiniteFloat(*pitch, keyframe.pitch))
            {
                diagnostics = "Camera keyframe values must be finite and frame must be an unsigned integer.";
                m_keyframes.clear();
                return false;
            }
            if (const cld::JsonValue* cut = cld::FindMember(value, "cut"))
            {
                if (cut->type != cld::JsonValue::Type::Bool)
                {
                    diagnostics = "Camera keyframe cut must be a boolean.";
                    m_keyframes.clear();
                    return false;
                }
                keyframe.cut = cut->boolean;
            }
            if (!m_keyframes.empty() && keyframe.frame <= m_keyframes.back().frame)
            {
                diagnostics = "Camera keyframe frames must be strictly increasing.";
                m_keyframes.clear();
                return false;
            }
            m_keyframes.push_back(keyframe);
        }
    }
    catch (const std::exception& exception)
    {
        m_keyframes.clear();
        diagnostics = std::string("Camera path JSON is invalid: ") + exception.what();
        return false;
    }

    diagnostics = "Camera path loaded.";
    return true;
}

bool CameraPath::Sample(std::uint32_t frame, CameraSample& sample, std::string& diagnostics) const
{
    if (m_keyframes.empty())
    {
        diagnostics = "Camera path is not loaded.";
        return false;
    }

    const auto upper = std::upper_bound(m_keyframes.begin(), m_keyframes.end(), frame,
        [](std::uint32_t value, const CameraKeyframe& keyframe)
        {
            return value < keyframe.frame;
        });
    if (upper == m_keyframes.begin())
    {
        const CameraKeyframe& keyframe = m_keyframes.front();
        sample = { keyframe.position, keyframe.yaw, keyframe.pitch, frame == keyframe.frame && keyframe.cut };
        diagnostics = "Camera sampled.";
        return true;
    }

    const CameraKeyframe& lower = *(upper - 1);
    if (lower.frame == frame || upper == m_keyframes.end())
    {
        sample = { lower.position, lower.yaw, lower.pitch, lower.frame == frame && lower.cut };
        diagnostics = "Camera sampled.";
        return true;
    }
    if (upper->cut)
    {
        sample = { lower.position, lower.yaw, lower.pitch, false };
        diagnostics = "Camera sampled.";
        return true;
    }

    const float amount = static_cast<float>(frame - lower.frame) / static_cast<float>(upper->frame - lower.frame);
    for (std::size_t axis = 0; axis < sample.position.size(); ++axis)
    {
        sample.position[axis] = lower.position[axis] + (upper->position[axis] - lower.position[axis]) * amount;
    }
    const double yawDelta = std::remainder(static_cast<double>(upper->yaw) - lower.yaw, 2.0 * Pi);
    sample.yaw = static_cast<float>(static_cast<double>(lower.yaw) + yawDelta * amount);
    sample.pitch = lower.pitch + (upper->pitch - lower.pitch) * amount;
    sample.cut = false;
    diagnostics = "Camera sampled.";
    return true;
}

Harness::Harness(Options options)
    : m_options(std::move(options))
{
}

bool Harness::Initialize(std::string& diagnostics)
{
    m_initialized = false;
    m_metrics.clear();
    m_metricNames.clear();
    m_artifacts.clear();
    const bool benchmarkKindValid = m_options.benchmarkKind == BenchmarkKind::Combined ||
        m_options.benchmarkKind == BenchmarkKind::Performance ||
        m_options.benchmarkKind == BenchmarkKind::Quality;
    if (!m_options.enabled || m_options.frames == 0 || m_options.cameraPath.empty() || m_options.outputDirectory.empty() ||
        static_cast<std::uint64_t>(m_options.frames) + m_options.warmup > MaximumFrameCount ||
        (m_options.captureAovs && m_options.captureEvery == 0) || !benchmarkKindValid)
    {
        diagnostics = "Benchmark options are incomplete or invalid.";
        return false;
    }
    if (!m_cameraPath.Load(m_options.cameraPath, diagnostics))
    {
        return false;
    }
    m_metrics.reserve(TotalFrames());
    m_initialized = true;
    diagnostics = "Benchmark harness initialized.";
    return true;
}

std::uint32_t Harness::TotalFrames() const
{
    return m_options.warmup + m_options.frames;
}

bool Harness::ShouldCaptureFrame(std::uint32_t frameIndex) const
{
    if (!m_initialized || m_options.captureEvery == 0 || frameIndex < m_options.warmup || frameIndex >= TotalFrames())
    {
        return false;
    }
    const std::uint32_t measuredFrameIndex = frameIndex - m_options.warmup;
    return measuredFrameIndex % m_options.captureEvery == 0;
}

bool Harness::GetFramePlan(std::uint32_t frameIndex, FramePlan& plan, std::string& diagnostics) const
{
    if (!m_initialized || frameIndex >= TotalFrames())
    {
        diagnostics = "Benchmark frame index is outside the active run.";
        return false;
    }
    plan = {};
    plan.frameIndex = frameIndex;
    plan.warmup = frameIndex < m_options.warmup;
    plan.measuredFrameIndex = plan.warmup ? InvalidMeasuredFrameIndex : frameIndex - m_options.warmup;
    plan.deltaSeconds = FixedDeltaSeconds;
    plan.elapsedSeconds = static_cast<double>(frameIndex) * FixedDeltaSeconds;
    return m_cameraPath.Sample(frameIndex, plan.camera, diagnostics);
}

bool Harness::RecordFrameMetrics(std::uint32_t frameIndex, const MetricValues& values, std::string& diagnostics)
{
    if (!m_initialized || frameIndex >= TotalFrames() || frameIndex != m_metrics.size())
    {
        diagnostics = "Benchmark metrics must be recorded once in strictly increasing frame order.";
        return false;
    }
    if (values.empty() || values.size() > 128)
    {
        diagnostics = "Benchmark frames require between 1 and 128 metrics.";
        return false;
    }

    std::vector<std::string> names;
    names.reserve(values.size());
    for (const auto& [name, value] : values)
    {
        if (!IsValidMetricName(name) || !std::isfinite(value))
        {
            diagnostics = "Benchmark metric names and values must be valid and finite.";
            return false;
        }
        names.push_back(name);
    }
    if (m_metricNames.empty())
    {
        m_metricNames = names;
    }
    else if (names != m_metricNames)
    {
        diagnostics = "Every benchmark frame must provide the same metric set.";
        return false;
    }

    m_metrics.push_back({ frameIndex, values });
    diagnostics = "Benchmark frame metrics recorded.";
    return true;
}

bool Harness::RegisterArtifact(const ArtifactRecord& artifact, std::string& diagnostics)
{
    if (!m_initialized || !IsSafeArtifactPath(artifact.relativePath) ||
        !IsArtifactToken(artifact.role) || !IsArtifactToken(artifact.encoding) || !IsArtifactToken(artifact.sourceFormat) ||
        artifact.width == 0 || artifact.height == 0)
    {
        diagnostics = "Benchmark artifact metadata is invalid.";
        return false;
    }
    if (artifact.statistics.available)
    {
        const std::uint64_t pixelCount = static_cast<std::uint64_t>(artifact.width) * artifact.height;
        if (artifact.statistics.channelCount == 0 || artifact.statistics.channelCount > 4 ||
            artifact.statistics.nonFinitePixelCount > pixelCount ||
            artifact.statistics.nonFiniteValueCount > pixelCount * artifact.statistics.channelCount)
        {
            diagnostics = "Benchmark artifact statistics are invalid.";
            return false;
        }
        for (std::uint32_t channel = 0; channel < artifact.statistics.channelCount; ++channel)
        {
            if (!std::isfinite(artifact.statistics.channelMin[channel]) ||
                !std::isfinite(artifact.statistics.channelMax[channel]) ||
                artifact.statistics.channelMin[channel] > artifact.statistics.channelMax[channel])
            {
                diagnostics = "Benchmark artifact min/max statistics must be finite and ordered.";
                return false;
            }
        }
    }

    ArtifactRecord normalized = artifact;
    normalized.relativePath = artifact.relativePath.lexically_normal();
    if (normalized.phase == ArtifactPhase::MeasuredFrame)
    {
        if (normalized.frameIndex >= TotalFrames() || normalized.frameIndex < m_options.warmup ||
            normalized.measuredFrameIndex != normalized.frameIndex - m_options.warmup ||
            !ShouldCaptureFrame(normalized.frameIndex))
        {
            diagnostics = "Sequence artifact frame metadata does not match the capture schedule.";
            return false;
        }
    }
    else if (normalized.frameIndex != InvalidMeasuredFrameIndex ||
        normalized.measuredFrameIndex != InvalidMeasuredFrameIndex)
    {
        diagnostics = "Final benchmark artifacts must not claim a frame index.";
        return false;
    }

    const auto duplicate = std::find_if(m_artifacts.begin(), m_artifacts.end(), [&](const ArtifactRecord& existing)
    {
        return existing.relativePath == normalized.relativePath ||
            (existing.role == normalized.role && existing.phase == normalized.phase &&
                existing.frameIndex == normalized.frameIndex &&
                existing.measuredFrameIndex == normalized.measuredFrameIndex);
    });
    if (duplicate != m_artifacts.end())
    {
        diagnostics = "Benchmark artifact path was registered more than once.";
        return false;
    }

    m_artifacts.push_back(std::move(normalized));
    diagnostics = "Benchmark artifact registered.";
    return true;
}

bool Harness::WriteOutputs(std::string& diagnostics) const
{
    if (!m_initialized || m_metrics.size() != TotalFrames() || m_metricNames.empty())
    {
        diagnostics = "Benchmark outputs require metrics for every warmup and measured frame.";
        return false;
    }

    std::error_code directoryError;
    std::filesystem::create_directories(m_options.outputDirectory, directoryError);
    if (directoryError)
    {
        diagnostics = "Could not create benchmark output directory: " + directoryError.message();
        return false;
    }

    struct HashedArtifact
    {
        const ArtifactRecord* metadata = nullptr;
        std::uintmax_t fileBytes = 0;
        std::string sha256;
    };
    std::vector<HashedArtifact> hashedArtifacts;
    hashedArtifacts.reserve(m_artifacts.size());
    for (const ArtifactRecord& artifact : m_artifacts)
    {
        HashedArtifact hashed;
        hashed.metadata = &artifact;
        if (!HashFileSha256(
            m_options.outputDirectory / artifact.relativePath,
            hashed.sha256,
            hashed.fileBytes,
            diagnostics))
        {
            return false;
        }
        hashedArtifacts.push_back(std::move(hashed));
    }
    const bool artifactStatisticsComplete = !m_artifacts.empty() &&
        std::all_of(m_artifacts.begin(), m_artifacts.end(), [](const ArtifactRecord& artifact)
        {
            return artifact.statistics.available;
        });
    std::uint64_t artifactNonFiniteValues = 0;
    std::uint64_t artifactNonFinitePixels = 0;
    for (const ArtifactRecord& artifact : m_artifacts)
    {
        if (artifact.statistics.available)
        {
            artifactNonFiniteValues += artifact.statistics.nonFiniteValueCount;
            artifactNonFinitePixels += artifact.statistics.nonFinitePixelCount;
        }
    }
    const bool artifactAllFinite = artifactStatisticsComplete && artifactNonFiniteValues == 0;

    std::ostringstream csv;
    csv.imbue(std::locale::classic());
    csv << "frame_index,measured_frame,phase,fixed_delta_seconds";
    for (const std::string& name : m_metricNames)
    {
        csv << ',' << EscapeCsv(name);
    }
    csv << '\n' << std::setprecision(17);
    for (const FrameMetrics& frame : m_metrics)
    {
        const bool warmup = frame.frameIndex < m_options.warmup;
        csv << frame.frameIndex << ',';
        if (warmup) csv << -1;
        else csv << frame.frameIndex - m_options.warmup;
        csv << ',' << (warmup ? "warmup" : "measured") << ',' << FixedDeltaSeconds;
        for (const std::string& name : m_metricNames)
        {
            csv << ',' << frame.values.at(name);
        }
        csv << '\n';
    }

    struct MetricSummary
    {
        std::size_t count = 0;
        double median = 0.0;
        double p95 = 0.0;
        double p99 = 0.0;
    };
    std::map<std::string, MetricSummary> metricSummaries;
    for (const std::string& name : m_metricNames)
    {
        std::vector<double> values;
        values.reserve(m_options.frames);
        for (std::size_t frame = m_options.warmup; frame < m_metrics.size(); ++frame)
        {
            values.push_back(m_metrics[frame].values.at(name));
        }
        std::sort(values.begin(), values.end());
        metricSummaries.emplace(name, MetricSummary{
            values.size(),
            Percentile(values, 0.50),
            Percentile(values, 0.95),
            Percentile(values, 0.99) });
    }

    const auto metric = [&](const char* name) -> const MetricSummary&
    {
        const auto found = metricSummaries.find(name);
        if (found == metricSummaries.end())
        {
            throw std::runtime_error(std::string("Required benchmark metric is missing: ") + name);
        }
        return found->second;
    };

    bool timingAssociationValid = true;
    bool native1080p = true;
    bool targetAdapter = true;
    bool targetScene = true;
    bool interactiveProfile = true;
    bool beautyView = true;
    bool finalTaaActive = true;
    bool requestedBackends = true;
    bool activeBackends = true;
    bool targetRayBudget = true;
    for (std::size_t frame = m_options.warmup; frame < m_metrics.size(); ++frame)
    {
        const MetricValues& values = m_metrics[frame].values;
        timingAssociationValid = timingAssociationValid &&
            values.at("gpu_timing_valid") >= 0.5 &&
            std::abs(values.at("submission_serial") - values.at("gpu_timing_serial")) < 0.5;
        native1080p = native1080p &&
            std::abs(values.at("render_width") - 1920.0) < 0.5 &&
            std::abs(values.at("render_height") - 1080.0) < 0.5;
        targetAdapter = targetAdapter && values.at("target_adapter_rtx_4070") >= 0.5;
        targetScene = targetScene && values.at("target_scene_bistro") >= 0.5;
        interactiveProfile = interactiveProfile && values.at("quality_profile_interactive") >= 0.5;
        beautyView = beautyView && values.at("beauty_view") >= 0.5;
        finalTaaActive = finalTaaActive && values.at("final_taa_active") >= 0.5;
        requestedBackends = requestedBackends &&
            values.at("requested_denoiser_nrd_reblur") >= 0.5 &&
            values.at("requested_restir_rtxdi_combined") >= 0.5;
        activeBackends = activeBackends &&
            values.at("active_denoiser_nrd_reblur") >= 0.5 &&
            values.at("active_restir_rtxdi_combined") >= 0.5;
        targetRayBudget = targetRayBudget &&
            std::abs(values.at("budget_moving_spp") - 1.0) < 0.5 &&
            std::abs(values.at("budget_moving_bounces") - 2.0) < 0.5 &&
            std::abs(values.at("budget_static_base_spp") - 1.0) < 0.5 &&
            std::abs(values.at("budget_static_max_spp") - 2.0) < 0.5 &&
            std::abs(values.at("budget_static_bounces") - 4.0) < 0.5 &&
            std::abs(values.at("budget_settle_frames") - 8.0) < 0.5 &&
            std::abs(values.at("budget_target_gpu_ms") - 14.5) < 0.01;
    }
    const bool officialWindow = m_options.warmup >= 120u && m_options.frames >= 300u;
    const bool targetConfiguration = targetAdapter && targetScene && interactiveProfile && beautyView &&
        finalTaaActive && requestedBackends && activeBackends && targetRayBudget;
    const bool gpuP95Passed = timingAssociationValid && metric("gpu_pipeline_ms").p95 <= 16.7;
    const bool gpuP99Passed = timingAssociationValid && metric("gpu_pipeline_ms").p99 <= 20.0;
    const bool cpuP95Passed = metric("cpu_frame_ms").p95 <= 4.0;
    const bool memoryPassed = metric("frame_history_mib").p99 <= 512.0;
    // Combined runs preserve the historical all-counters output and quality
    // runs intentionally execute diagnostic work. Only an isolated
    // performance run is eligible for the official frame-time gate.
    const bool isolatedPerformanceRun = m_options.benchmarkKind == BenchmarkKind::Performance;
    const bool eligible = isolatedPerformanceRun && officialWindow && native1080p &&
        timingAssociationValid && targetConfiguration;
    const bool performancePassed = eligible && gpuP95Passed && gpuP99Passed && cpuP95Passed && memoryPassed;

    std::ostringstream summary;
    summary.imbue(std::locale::classic());
    summary << std::setprecision(17)
            << "{\n  \"schemaVersion\": 2,\n"
            << "  \"benchmarkKind\": \"" << BenchmarkKindName(m_options.benchmarkKind) << "\",\n"
            << "  \"fixedDeltaSeconds\": " << FixedDeltaSeconds << ",\n"
            << "  \"seed\": " << m_options.seed << ",\n"
            << "  \"warmupFrames\": " << m_options.warmup << ",\n"
            << "  \"measuredFrames\": " << m_options.frames << ",\n"
            << "  \"recordedFrames\": " << m_metrics.size() << ",\n"
            << "  \"cameraKeyframes\": " << m_cameraPath.Keyframes().size() << ",\n"
            << "  \"cameraPath\": \"" << cld::EscapeJson(PathToUtf8(m_options.cameraPath)) << "\",\n"
            << "  \"artifactManifest\": \"artifacts.json\",\n"
            << "  \"qualityAnalysisContract\": \"quality_analysis.json\",\n"
            << "  \"artifactQualityGate\": {\"evaluated\": " << (artifactStatisticsComplete ? "true" : "false")
            << ", \"allFinite\": " << (artifactAllFinite ? "true" : "false")
            << ", \"nonFiniteValues\": " << artifactNonFiniteValues
            << ", \"nonFinitePixels\": " << artifactNonFinitePixels << "},\n"
            << "  \"metrics\": {\n";
    for (std::size_t metricIndex = 0; metricIndex < m_metricNames.size(); ++metricIndex)
    {
        const std::string& name = m_metricNames[metricIndex];
        const MetricSummary& statistics = metricSummaries.at(name);
        summary << "    \"" << cld::EscapeJson(name) << "\": {\"count\": " << statistics.count
                << ", \"median\": " << statistics.median
                << ", \"p95\": " << statistics.p95
                << ", \"p99\": " << statistics.p99 << "}"
                << (metricIndex + 1 < m_metricNames.size() ? "," : "") << '\n';
    }
    summary << "  },\n"
            << "  \"metricSchema\": {\n"
            << "    \"recorded\": {\n";
    for (std::size_t metricIndex = 0; metricIndex < m_metricNames.size(); ++metricIndex)
    {
        const std::string& name = m_metricNames[metricIndex];
        const MetricDefinition& definition = DescribeMetric(name);
        summary << "      \"" << cld::EscapeJson(name) << "\": {\"unit\": \""
                << cld::EscapeJson(definition.unit) << "\", \"source\": \""
                << cld::EscapeJson(definition.source) << "\", \"description\": \""
                << cld::EscapeJson(definition.description) << "\"}"
                << (metricIndex + 1 < m_metricNames.size() ? "," : "") << '\n';
    }
    summary << "    },\n"
            << "    \"qualityCounters\": {\n"
            << "      \"abiVersion\": " << QualityCounterAbiVersion << ",\n"
            << "      \"tileRecordBytes\": " << sizeof(QualityCounterTileV1) << ",\n"
            << "      \"aggregation\": \"one-writer-per-tile_then_cpu-double-reduction\",\n"
            << "      \"metrics\": [\n";
    for (std::size_t contractIndex = 0; contractIndex < QualityMetricContracts.size(); ++contractIndex)
    {
        const QualityMetricContract& contract = QualityMetricContracts[contractIndex];
        const bool available = IncludesQuality(m_options.benchmarkKind) &&
            std::find(m_metricNames.begin(), m_metricNames.end(), contract.name) != m_metricNames.end();
        summary << "        {\"name\": \"" << contract.name << "\", \"unit\": \"" << contract.unit
                << "\", \"available\": " << (available ? "true" : "false")
                << ", \"description\": \"" << cld::EscapeJson(contract.description) << "\"}"
                << (contractIndex + 1 < QualityMetricContracts.size() ? "," : "") << '\n';
    }
    summary << "      ]\n"
            << "    }\n"
            << "  },\n"
            << "  \"performanceGate\": {\n"
            << "    \"eligible\": " << (eligible ? "true" : "false") << ",\n"
            << "    \"passed\": " << (performancePassed ? "true" : "false") << ",\n"
            << "    \"isolatedPerformanceRun\": " << (isolatedPerformanceRun ? "true" : "false") << ",\n"
            << "    \"officialWindow\": " << (officialWindow ? "true" : "false") << ",\n"
            << "    \"native1080p\": " << (native1080p ? "true" : "false") << ",\n"
            << "    \"timingAssociationValid\": " << (timingAssociationValid ? "true" : "false") << ",\n"
            << "    \"targetConfiguration\": " << (targetConfiguration ? "true" : "false") << ",\n"
            << "    \"targetAdapterRtx4070\": " << (targetAdapter ? "true" : "false") << ",\n"
            << "    \"targetSceneBistro\": " << (targetScene ? "true" : "false") << ",\n"
            << "    \"interactiveProfile\": " << (interactiveProfile ? "true" : "false") << ",\n"
            << "    \"beautyView\": " << (beautyView ? "true" : "false") << ",\n"
            << "    \"finalTaaActive\": " << (finalTaaActive ? "true" : "false") << ",\n"
            << "    \"requestedTargetBackends\": " << (requestedBackends ? "true" : "false") << ",\n"
            << "    \"activeTargetBackends\": " << (activeBackends ? "true" : "false") << ",\n"
            << "    \"targetRayBudget\": " << (targetRayBudget ? "true" : "false") << ",\n"
            << "    \"gpuP95Within16_7Ms\": " << (gpuP95Passed ? "true" : "false") << ",\n"
            << "    \"gpuP99Within20Ms\": " << (gpuP99Passed ? "true" : "false") << ",\n"
            << "    \"cpuP95Within4Ms\": " << (cpuP95Passed ? "true" : "false") << ",\n"
            << "    \"frameHistoryWithin512MiB\": " << (memoryPassed ? "true" : "false") << "\n"
            << "  }\n}\n";

    std::ostringstream artifactManifest;
    artifactManifest.imbue(std::locale::classic());
    artifactManifest << std::setprecision(17) << "{\n"
        << "  \"schemaVersion\": 1,\n"
        << "  \"hashAlgorithm\": \"sha256\",\n"
        << "  \"capture\": {\"sequenceEnabled\": " << (m_options.captureEvery > 0 ? "true" : "false")
        << ", \"everyMeasuredFrames\": " << m_options.captureEvery
        << ", \"includeAovs\": " << (m_options.captureAovs ? "true" : "false") << "},\n"
        << "  \"validation\": {\"statisticsComplete\": " << (artifactStatisticsComplete ? "true" : "false")
        << ", \"allFinite\": " << (artifactAllFinite ? "true" : "false")
        << ", \"nonFiniteValues\": " << artifactNonFiniteValues
        << ", \"nonFinitePixels\": " << artifactNonFinitePixels << "},\n"
        << "  \"artifacts\": [\n";
    for (std::size_t artifactIndex = 0; artifactIndex < hashedArtifacts.size(); ++artifactIndex)
    {
        const HashedArtifact& hashed = hashedArtifacts[artifactIndex];
        const ArtifactRecord& artifact = *hashed.metadata;
        artifactManifest << "    {\"path\": \"" << cld::EscapeJson(PathToUtf8(artifact.relativePath))
            << "\", \"role\": \"" << cld::EscapeJson(artifact.role)
            << "\", \"phase\": \"" << (artifact.phase == ArtifactPhase::Final ? "final" : "measured") << "\", ";
        if (artifact.phase == ArtifactPhase::Final)
        {
            artifactManifest << "\"frameIndex\": null, \"measuredFrameIndex\": null, ";
        }
        else
        {
            artifactManifest << "\"frameIndex\": " << artifact.frameIndex
                << ", \"measuredFrameIndex\": " << artifact.measuredFrameIndex << ", ";
        }
        artifactManifest << "\"width\": " << artifact.width << ", \"height\": " << artifact.height
            << ", \"sourceFormat\": \"" << cld::EscapeJson(artifact.sourceFormat)
            << "\", \"encoding\": \"" << cld::EscapeJson(artifact.encoding)
            << "\", \"fileBytes\": " << hashed.fileBytes
            << ", \"sha256\": \"" << hashed.sha256 << "\", \"statistics\": ";
        if (!artifact.statistics.available)
        {
            artifactManifest << "null";
        }
        else
        {
            artifactManifest << "{\"channelCount\": " << artifact.statistics.channelCount
                << ", \"nonFiniteValues\": " << artifact.statistics.nonFiniteValueCount
                << ", \"nonFinitePixels\": " << artifact.statistics.nonFinitePixelCount
                << ", \"min\": [";
            for (std::uint32_t channel = 0; channel < artifact.statistics.channelCount; ++channel)
            {
                artifactManifest << artifact.statistics.channelMin[channel]
                    << (channel + 1u < artifact.statistics.channelCount ? ", " : "");
            }
            artifactManifest << "], \"max\": [";
            for (std::uint32_t channel = 0; channel < artifact.statistics.channelCount; ++channel)
            {
                artifactManifest << artifact.statistics.channelMax[channel]
                    << (channel + 1u < artifact.statistics.channelCount ? ", " : "");
            }
            artifactManifest << "]}";
        }
        artifactManifest << "}"
            << (artifactIndex + 1 < hashedArtifacts.size() ? "," : "") << '\n';
    }
    artifactManifest << "  ]\n}\n";

    std::vector<const ArtifactRecord*> temporalInputs;
    for (const ArtifactRecord& artifact : m_artifacts)
    {
        if (artifact.phase == ArtifactPhase::MeasuredFrame && artifact.role == "beauty_hdr")
        {
            temporalInputs.push_back(&artifact);
        }
    }
    std::sort(temporalInputs.begin(), temporalInputs.end(), [](const ArtifactRecord* left, const ArtifactRecord* right)
    {
        return left->measuredFrameIndex < right->measuredFrameIndex;
    });
    bool temporalCvReady = m_options.captureEvery == 1 && temporalInputs.size() == m_options.frames && temporalInputs.size() >= 2;
    for (std::size_t inputIndex = 0; temporalCvReady && inputIndex < temporalInputs.size(); ++inputIndex)
    {
        temporalCvReady = temporalInputs[inputIndex]->measuredFrameIndex == inputIndex;
    }

    std::ostringstream analysisContract;
    analysisContract.imbue(std::locale::classic());
    analysisContract << "{\n"
        << "  \"schemaVersion\": 1,\n"
        << "  \"artifactManifest\": \"artifacts.json\",\n"
        << "  \"temporalCv\": {\n"
        << "    \"ready\": " << (temporalCvReady ? "true" : "false") << ",\n"
        << "    \"sourceRole\": \"beauty_hdr\",\n"
        << "    \"requireContiguousMeasuredFrames\": true,\n"
        << "    \"luminance\": \"Rec.709 linear: 0.2126 R + 0.7152 G + 0.0722 B\",\n"
        << "    \"definition\": \"per-pixel population standard deviation divided by max(mean, 1e-6)\",\n"
        << "    \"aggregate\": [\"median\", \"p95\"],\n"
        << "    \"targets\": {\"medianMax\": 0.01, \"p95Max\": 0.03},\n"
        << "    \"inputs\": [";
    for (std::size_t inputIndex = 0; inputIndex < temporalInputs.size(); ++inputIndex)
    {
        analysisContract << "\"" << cld::EscapeJson(PathToUtf8(temporalInputs[inputIndex]->relativePath)) << "\""
            << (inputIndex + 1 < temporalInputs.size() ? ", " : "");
    }
    analysisContract << "]\n"
        << "  },\n"
        << "  \"edgeWidth\": {\n"
        << "    \"ready\": false,\n"
        << "    \"sourceRole\": \"beauty_hdr\",\n"
        << "    \"referenceManifest\": null,\n"
        << "    \"roi\": null,\n"
        << "    \"definition\": \"10-90 percent linear-luminance edge-spread width perpendicular to a configured edge ROI\",\n"
        << "    \"outputMetric\": \"edge_width_ratio_to_reference\",\n"
        << "    \"targetMax\": 1.15,\n"
        << "    \"setup\": \"Set referenceManifest and roi in a copied analysis request before post-processing.\"\n"
        << "  }\n"
        << "}\n";

    if (!WriteTextFile(m_options.outputDirectory / "frames.csv", csv.str(), diagnostics) ||
        !WriteTextFile(m_options.outputDirectory / "summary.json", summary.str(), diagnostics) ||
        !WriteTextFile(m_options.outputDirectory / "artifacts.json", artifactManifest.str(), diagnostics) ||
        !WriteTextFile(m_options.outputDirectory / "quality_analysis.json", analysisContract.str(), diagnostics))
    {
        return false;
    }
    diagnostics = "Benchmark CSV, summary, artifact manifest, and quality-analysis contract written.";
    return true;
}
}
