#include "stdafx.h"
#include "RtxdiBackend.h"

#if D3D12LOOKDEVPT_WITH_RTXDI
#include <Rtxdi/GI/ReSTIRGIParameters.h>
#include <Rtxdi/PT/ReSTIRPTParameters.h>
#include <Rtxdi/RtxdiParameters.h>
#include <Rtxdi/RtxdiUtils.h>

static_assert(sizeof(RTXDI_RuntimeParameters) == 16, "Unexpected RTXDI v3.0.0 ABI.");
static_assert(sizeof(RTXDI_PackedDIReservoir) == 24, "Unexpected RTXDI packed DI reservoir ABI.");
static_assert(sizeof(RTXDI_PackedGIReservoir) == 32, "Unexpected RTXDI packed GI reservoir ABI.");
static_assert(sizeof(RTXDI_PackedPTReservoir) == 64, "Unexpected RTXDI packed PT reservoir ABI.");
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
    m_status.ptEvaluationReady = false;
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
        ? "RTXDI runtime and DI ABI are available; GI/PT readiness is gated by the selected renderer pipelines."
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

void RtxdiBackendRuntime::SetRendererEvaluationReady(bool giReady, bool ptReady)
{
    m_status.giEvaluationReady = m_status.runtimeLibraryLinked && giReady;
    m_status.ptEvaluationReady = m_status.runtimeLibraryLinked && ptReady;
    m_status.evaluationReady =
        m_status.diEvaluationReady || m_status.giEvaluationReady || m_status.ptEvaluationReady;
    if (m_status.giEvaluationReady)
    {
        m_status.fallbackReason = m_status.ptEvaluationReady
            ? ""
            : "RTXDI ReSTIR DI/GI is available. ReSTIR PT remains on the Baseline PT fallback.";
    }
    else if (m_status.ptEvaluationReady)
    {
        m_status.fallbackReason = "";
    }
}

RtxdiReservoirLayout RtxdiBackendRuntime::CalculateGiReservoirLayout(
    std::uint32_t width,
    std::uint32_t height) const
{
    RtxdiReservoirLayout layout = CalculateReservoirLayout(width, height);
#if D3D12LOOKDEVPT_WITH_RTXDI
    layout.elementStride = static_cast<std::uint32_t>(sizeof(RTXDI_PackedGIReservoir));
#endif
    return layout;
}

RtxdiReservoirLayout RtxdiBackendRuntime::CalculatePtReservoirLayout(
    std::uint32_t width,
    std::uint32_t height) const
{
    RtxdiReservoirLayout layout = CalculateReservoirLayout(width, height);
#if D3D12LOOKDEVPT_WITH_RTXDI
    // Interactive ReSTIR PT alternates checkerboard fields. Both fields use
    // the same half-width packed layout; frame parity selects which pixels map
    // into it and the A/B resources preserve the opposite field as history.
    const RTXDI_ReservoirBufferParameters parameters = rtxdi::CalculateReservoirBufferParameters(
        (std::max)(width, 1u),
        (std::max)(height, 1u),
        rtxdi::CheckerboardMode::Black);
    layout.blockRowPitch = parameters.reservoirBlockRowPitch;
    layout.arrayPitch = parameters.reservoirArrayPitch;
    layout.elementStride = static_cast<std::uint32_t>(sizeof(RTXDI_PackedPTReservoir));
#endif
    return layout;
}
