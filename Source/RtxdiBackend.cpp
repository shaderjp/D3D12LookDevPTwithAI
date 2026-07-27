#include "stdafx.h"
#include "RtxdiBackend.h"

#if D3D12LOOKDEVPT_WITH_RTXDI
#include <Rtxdi/RtxdiParameters.h>
#include <Rtxdi/RtxdiUtils.h>

static_assert(sizeof(RTXDI_RuntimeParameters) == 16, "Unexpected RTXDI v3.0.0 ABI.");
static_assert(sizeof(RTXDI_PackedDIReservoir) == 24, "Unexpected RTXDI packed DI reservoir ABI.");
#endif

RtxdiBackendRuntime::RtxdiBackendRuntime()
{
    RefreshStatus();
}

void RtxdiBackendRuntime::RefreshStatus()
{
    m_status = {};
    m_status.requestedAtBuild = D3D12LOOKDEVPT_RTXDI_REQUESTED != 0;
    m_status.compiled = D3D12LOOKDEVPT_WITH_RTXDI != 0;
    m_status.sdkAvailable = D3D12LOOKDEVPT_RTXDI_SDK_AVAILABLE != 0;
    m_status.evaluationReady = false;
    m_status.diEvaluationReady = false;
    m_status.giEvaluationReady = false;
    m_status.active = false;

#if D3D12LOOKDEVPT_WITH_RTXDI
    const RTXDI_ReservoirBufferParameters parameters = rtxdi::CalculateReservoirBufferParameters(
        1,
        1,
        rtxdi::CheckerboardMode::Off);
    m_status.runtimeLibraryLinked = parameters.reservoirBlockRowPitch > 0 && parameters.reservoirArrayPitch > 0;
    m_status.diEvaluationReady = m_status.runtimeLibraryLinked;
    m_status.evaluationReady = m_status.diEvaluationReady;
    m_status.fallbackReason = m_status.diEvaluationReady
        ? "RTXDI ReSTIR DI is available. ReSTIR GI/PT is not integrated and remains on the Baseline PT fallback."
        : "RTXDI v3.0.0 runtime layout validation failed; using Baseline PT fallback.";
#else
    m_status.runtimeLibraryLinked = false;
    if (m_status.requestedAtBuild)
    {
        m_status.fallbackReason =
            "EnableRTXDI was requested, but the pinned RTXDI v3.0.0 SDK/runtime was not found under ThirdParty/RTXDI; using Baseline PT fallback.";
    }
    else
    {
        m_status.fallbackReason = "RTXDI was disabled at build time; using Baseline PT fallback.";
    }
#endif
}

RtxdiReservoirLayout RtxdiBackendRuntime::CalculateReservoirLayout(
    std::uint32_t width,
    std::uint32_t height) const
{
    RtxdiReservoirLayout layout{};
#if D3D12LOOKDEVPT_WITH_RTXDI
    const RTXDI_ReservoirBufferParameters parameters = rtxdi::CalculateReservoirBufferParameters(
        (std::max)(width, 1u),
        (std::max)(height, 1u),
        rtxdi::CheckerboardMode::Off);
    layout.blockRowPitch = parameters.reservoirBlockRowPitch;
    layout.arrayPitch = parameters.reservoirArrayPitch;
    layout.elementStride = static_cast<std::uint32_t>(sizeof(RTXDI_PackedDIReservoir));
#else
    (void)width;
    (void)height;
#endif
    return layout;
}
