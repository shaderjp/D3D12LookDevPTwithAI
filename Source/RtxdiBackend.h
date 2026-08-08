#pragma once

#include <cstdint>
#include <string>

#ifndef D3D12LOOKDEVPT_RTXDI_REQUESTED
#define D3D12LOOKDEVPT_RTXDI_REQUESTED 0
#endif

#ifndef D3D12LOOKDEVPT_WITH_RTXDI
#define D3D12LOOKDEVPT_WITH_RTXDI 0
#endif

#ifndef D3D12LOOKDEVPT_RTXDI_SDK_AVAILABLE
#define D3D12LOOKDEVPT_RTXDI_SDK_AVAILABLE 0
#endif

inline constexpr const char* D3D12LookDevPtRtxdiVersion = "3.0.0";
inline constexpr const char* D3D12LookDevPtRtxdiCommit = "274141af082050c9d0ad6e01a2e591d0d66b7955";
inline constexpr const char* D3D12LookDevPtRtxdiRuntimeCommit = "a14e079c727ed8c4fd3173bd2aea8244c9d9f6d6";

struct RtxdiStatus
{
    bool requestedAtBuild = false;
    bool compiled = false;
    bool sdkAvailable = false;
    bool runtimeLibraryLinked = false;
    bool evaluationReady = false;
    bool diEvaluationReady = false;
    bool giEvaluationReady = false;
    bool ptEvaluationReady = false;
    bool active = false;
    std::string sdkVersion = D3D12LookDevPtRtxdiVersion;
    std::string sdkCommit = D3D12LookDevPtRtxdiCommit;
    std::string runtimeCommit = D3D12LookDevPtRtxdiRuntimeCommit;
    std::string fallbackReason;
};

struct RtxdiReservoirLayout
{
    std::uint32_t blockRowPitch = 1;
    std::uint32_t arrayPitch = 1;
    std::uint32_t elementStride = 24;
};

// Optional RTXDI SDK/build boundary. The renderer owns D3D12 resources and
// dispatches, while this class is the single source of truth for the pinned
// runtime ABI and the official block-linear reservoir layout.
class RtxdiBackendRuntime
{
public:
    RtxdiBackendRuntime();

    void RefreshStatus();
    const RtxdiStatus& Status() const { return m_status; }
    bool IsEvaluationReady() const { return m_status.evaluationReady; }
    bool IsDiEvaluationReady() const { return m_status.diEvaluationReady; }
    bool IsGiEvaluationReady() const { return m_status.giEvaluationReady; }
    bool IsPtEvaluationReady() const { return m_status.ptEvaluationReady; }
    void SetRendererEvaluationReady(bool giReady, bool ptReady);
    RtxdiReservoirLayout CalculateReservoirLayout(std::uint32_t width, std::uint32_t height) const;
    RtxdiReservoirLayout CalculateGiReservoirLayout(std::uint32_t width, std::uint32_t height) const;
    RtxdiReservoirLayout CalculatePtReservoirLayout(std::uint32_t width, std::uint32_t height) const;

private:
    RtxdiStatus m_status;
};
