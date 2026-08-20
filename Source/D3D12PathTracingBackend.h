#pragma once

#include "DXSample.h"
#include "BenchmarkHarness.h"
#include "DlssBackend.h"
#include "FpsCamera.h"
#include "McpDispatcher.h"
#include "McpReviewAnalysis.h"
#include "McpServer.h"
#include "NrdBackend.h"
#include "PathTracingScene.h"
#include "QualitySettingsJson.h"
#include "RenderStabilityTypes.h"
#include "RtxdiBackend.h"
#include "SceneImporter.h"
#include "SimpleJson.h"
#include "WinUI/WinUIEditor.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>

using namespace DirectX;
using Microsoft::WRL::ComPtr;

enum class PathTracingMode
{
    Pathtracing,
    ReSTIR,
    ReSTIRDI,
    ReSTIRCombined,
    ReSTIRPT,
    ReSTIRPTCombined,
};

class D3D12PathTracingBackend : public DXSample, public mcp::IServerHost
{
public:
    D3D12PathTracingBackend(UINT width, UINT height, std::wstring name, PathTracingMode mode);

    void OnInit() override;
    void OnUpdate() override;
    void OnRender() override;
    void OnDestroy() override;
    void OnKeyDown(UINT8 key) override;
    void OnWindowMessage(UINT message, WPARAM wParam, LPARAM lParam);
    void ParseCommandLineArgs(_In_reads_(argc) WCHAR* argv[], int argc) override;
    const rb::FrameState& GetFrameState() const noexcept { return m_frameState; }
    const rb::RayBudgetState& GetRayBudgetState() const noexcept { return m_rayBudgetState; }
    float GetActiveSecondaryShadingRate() const noexcept { return m_activeSecondaryShadingRate; }
    ComPtr<IDXGISwapChain3> GetCompositionSwapChain() const noexcept { return m_swapChain; }
    bool IsBenchmarkFinished() const noexcept { return m_benchmarkFinished; }
    void SetViewportFocused(bool focused) noexcept;
    void ApplyEditorCommand(const lookdevpt::winui::EditorCommand& command);
    lookdevpt::winui::EditorSnapshot CaptureEditorSnapshot() const;
    void AddEditorCpuTime(double milliseconds) noexcept;

    enum class NoisePreset
    {
        InteractiveStable,
        SharpPreview,
        StillCapture,
    };

    enum class JitterMode
    {
        Stable32,
        Halton,
        Off,
    };

    enum class DenoiseBackend
    {
        Internal,
        NrdReblur,
        NrdRelax,
        DlssRayReconstruction,
        Off,
    };

    enum class ToneMapper
    {
        None,
        Reinhard,
        Aces,
    };

    enum class MaterialFocusMode
    {
        Normal,
        Isolate,
        Dim,
    };

private:
    static const UINT FrameCount = 3;
    static const UINT TextureSlotCount = Bistro::TextureSlotCount;
    static const UINT DenoiseAtrousPipelineCount = 5;
    // Keep every full-screen/temporal stage on its own timestamp boundary.
    // Besides making PIX captures easier to correlate, this prevents quality
    // diagnostics and history publication from being charged to the denoiser.
    static const UINT GpuTimestampCount = 19;

    enum GpuTimestamp : UINT
    {
        GpuTimestampFrameBegin = 0,
        GpuTimestampAfterPathTrace,
        GpuTimestampAfterRestirCandidate,
        GpuTimestampAfterRestirTemporal,
        GpuTimestampAfterRestirSpatial,
        GpuTimestampAfterRestirShade,
        GpuTimestampAfterRestirPublish,
        GpuTimestampAfterGiInitial,
        GpuTimestampAfterGiFused,
        GpuTimestampAfterPtInitial,
        GpuTimestampAfterPtFused,
        GpuTimestampAfterDenoisePrepare,
        GpuTimestampAfterDenoiseCore,
        GpuTimestampAfterDenoiseComposite,
        GpuTimestampAfterFinalTaa,
        GpuTimestampAfterQualityCounters,
        GpuTimestampAfterHistoryPublish,
        GpuTimestampAfterCopy,
        GpuTimestampFrameEnd,
    };

    enum DescriptorSlot : UINT
    {
        DescriptorOutputUav = 0,
        DescriptorAccumulationUav,
        DescriptorRestirCurrentUav,
        DescriptorRestirHistoryUav,
        DescriptorRestirSpatialUav,
        DescriptorDenoiseAov0Uav,
        DescriptorDenoiseAov1Uav,
        DescriptorDenoiseAov2Uav,
        DescriptorReconstructionHistoryRadianceUav,
        DescriptorReconstructionHistoryMomentsUav,
        DescriptorReconstructionHistoryLengthUav,
        DescriptorPreviousDenoiseAov0Uav,
        DescriptorPreviousDenoiseAov1Uav,
        DescriptorPreviousDenoiseAov2Uav,
        DescriptorRestirDiCurrentUav,
        DescriptorRestirDiHistoryUav,
        DescriptorRestirDiSpatialUav,
        DescriptorSignalCurrentRadianceUav,
        DescriptorSignalDirectUav,
        DescriptorSignalIndirectUav,
        DescriptorSignalResidualUav,
        DescriptorDenoisePingUav,
        DescriptorDenoisePongUav,
        DescriptorNrdMotionUav,
        DescriptorNrdNormalRoughnessUav,
        DescriptorNrdViewZUav,
        DescriptorNrdDiffRadianceHitDistanceUav,
        DescriptorNrdSpecRadianceHitDistanceUav,
        DescriptorNrdDiffDenoisedUav,
        DescriptorNrdSpecDenoisedUav,
        DescriptorReconstructionHistoryRadianceBUav,
        DescriptorReconstructionHistoryMomentsBUav,
        DescriptorReconstructionHistoryLengthBUav,
        DescriptorPostDenoiseHdrUav,
        DescriptorTaaHistoryAUav,
        DescriptorTaaHistoryBUav,
        DescriptorNrdDiffuseConfidenceUav,
        DescriptorNrdSpecularConfidenceUav,
        DescriptorSurfaceIdentityUav,
        DescriptorPreviousSurfaceIdentityUav,
        DescriptorFinalResolvedHdrUav,
        DescriptorQualityCounterUav,
        DescriptorQualityContributionUav,
        DescriptorRestirGiCurrentUav,
        DescriptorRestirGiHistoryUav,
        DescriptorRestirPtCurrentUav,
        DescriptorRestirPtHistoryUav,
        DescriptorDlssDepthUav,
        DescriptorDlssMotionUav,
        DescriptorDlssNormalRoughnessUav,
        DescriptorDlssAlbedoUav,
        DescriptorDlssSpecularAlbedoUav,
        DescriptorDlssExposureUav,
        DescriptorPrimaryPositionConeUav,
        DescriptorPrimaryGeometricNormalUav,
        DescriptorPrimaryIdentityUav,
        DescriptorSecondaryTaskOffsetsUav,
        DescriptorSecondaryGroupOffsetsUav,
        DescriptorSecondaryTasksUav,
        DescriptorSecondaryResultsUav,
        DescriptorSecondaryIndirectArgsUav,
        DescriptorReviewProbeUav,
        DescriptorVertexBuffer,
        DescriptorIndexBuffer,
        DescriptorGeometryBuffer,
        DescriptorMaterialBuffer,
        DescriptorLightBuffer,
        DescriptorInstanceBuffer,
        DescriptorTextureBase
    };

    // u0..u61 form one complete shader-visible output table. A second fixed
    // copy lives after the scene/texture descriptors and swaps only the
    // SurfaceGuide current/previous bindings for odd frames.
    static constexpr UINT OutputTableDescriptorCount = DescriptorVertexBuffer;
    static_assert(DescriptorDenoiseAov0Uav == 5u && DescriptorDenoiseAov2Uav == 7u,
        "SurfaceGuide current UAV ABI changed.");
    static_assert(DescriptorPreviousDenoiseAov0Uav == 11u && DescriptorPreviousDenoiseAov2Uav == 13u,
        "SurfaceGuide previous UAV ABI changed.");
    static_assert(DescriptorSurfaceIdentityUav == 38u && DescriptorPreviousSurfaceIdentityUav == 39u,
        "SurfaceGuide identity UAV ABI changed.");

    enum RootParameter : UINT
    {
        RootOutputTable = 0,
        RootAccelerationStructure,
        RootSceneConstants,
        RootSceneBuffers,
        RootTextureTable,
        RootParameterCount
    };

    enum class PendingFileDialog
    {
        None,
        OpenScene,
        OpenEnvironment,
        OpenProject,
        SaveProjectAs,
    };

    enum class SceneLoadStage : std::uint8_t
    {
        Idle,
        Parsing,
        LoadingAssets,
        BuildingBLAS,
        BuildingTLAS,
        Completed,
        Failed,
        Cancelled,
    };

    // UI edits are deferred until the next update tick. Ordering is deliberate:
    // coalescing requests keeps the most expansive refresh, while material and
    // texture edits avoid rebuilding stable frame/history and DXR resources.
    enum class PendingGpuResourceRefresh : std::uint8_t
    {
        None,
        MaterialData,
        MaterialTextures,
        FullScene,
    };

    struct SceneConstantBuffer
    {
        XMFLOAT4X4 inverseViewProjection;
        XMFLOAT4X4 viewProjection;
        XMFLOAT4X4 previousViewProjection;
        XMFLOAT4 cameraPosition;
        XMFLOAT4 lightDirection;
        XMFLOAT4 lightColor;
        XMFLOAT4 debugOptions;
        XMFLOAT4 skyColor;
        XMFLOAT4 skyHorizonColor;
        XMFLOAT4 skyZenithColor;
        XMFLOAT4 skyGroundColor;
        XMFLOAT4 skyOptions;
        XMFLOAT4 rayOptions;
        XMUINT4 frameOptions;
        XMFLOAT4 giOptions;
        XMFLOAT4 pathOptions;
        XMFLOAT4 restirOptions;
        XMFLOAT4 restirDiOptions;
        XMFLOAT4 lightOptions;
        XMFLOAT4 environmentOptions;
        XMFLOAT4 denoiseOptions;
        XMFLOAT4 denoiseOptions2;
        XMFLOAT4 jitterOptions;
        XMFLOAT4 reconstructionOptions;
        XMFLOAT4 validationOptions;
        XMFLOAT4 atrousOptions;
        XMFLOAT4 adaptiveOptions;
        XMFLOAT4 restirStabilityOptions;
        XMFLOAT4 signalDenoiseOptions;
        XMFLOAT4 denoisePassOptions;
        XMFLOAT4 stabilityOptions;
        XMFLOAT4 viewOptions;
        XMFLOAT4 materialFocusOptions;
        // x=camera ray cone spread, y=active secondary shading rate
        // (1.0 full, 0.5 adaptive half), z=quality diagnostics enabled.
        XMFLOAT4 performanceOptions;
        // x=NRD Composite + Final HDR TAA fusion for this submitted frame.
        XMFLOAT4 postProcessOptions;
        // x=power estimate for the emissive/analytic area-light family,
        // y=defensive uniform-family mixture fraction. Sun/environment weights
        // are evaluated from the live scene constants.
        XMFLOAT4 unifiedLightOptions;
        // xy=internal render dimensions, zw=display/output dimensions.
        // rayOptions.zw remains the canonical internal dispatch size.
        XMFLOAT4 renderOutputOptions;
        // Appended previous-frame camera state for reconstructing receiver
        // positions from the ping-ponged SurfaceGuide history.
        XMFLOAT4X4 previousInverseViewProjection;
        XMFLOAT4 previousCameraPosition;
        // Appended so the established fields above keep their ABI offsets.
        XMFLOAT4X4 environmentLightToWorld;
        XMFLOAT4X4 environmentWorldToLight;
        XMFLOAT4 environmentTint;
    };

    static_assert(sizeof(SceneConstantBuffer) == 960, "SceneConstants C++/HLSL ABI size changed.");
    static_assert(offsetof(SceneConstantBuffer, inverseViewProjection) == 0, "SceneConstants ABI mismatch.");
    static_assert(offsetof(SceneConstantBuffer, previousViewProjection) == 128, "SceneConstants ABI mismatch.");
    static_assert(offsetof(SceneConstantBuffer, cameraPosition) == 192, "SceneConstants ABI mismatch.");
    static_assert(offsetof(SceneConstantBuffer, materialFocusOptions) == 656, "SceneConstants ABI mismatch.");
    static_assert(offsetof(SceneConstantBuffer, performanceOptions) == 672, "SceneConstants ABI mismatch.");
    static_assert(offsetof(SceneConstantBuffer, postProcessOptions) == 688, "SceneConstants ABI mismatch.");
    static_assert(offsetof(SceneConstantBuffer, unifiedLightOptions) == 704, "SceneConstants ABI mismatch.");
    static_assert(offsetof(SceneConstantBuffer, renderOutputOptions) == 720, "SceneConstants ABI mismatch.");
    static_assert(offsetof(SceneConstantBuffer, previousInverseViewProjection) == 736, "SceneConstants ABI mismatch.");
    static_assert(offsetof(SceneConstantBuffer, previousCameraPosition) == 800, "SceneConstants ABI mismatch.");
    static_assert(offsetof(SceneConstantBuffer, environmentLightToWorld) == 816, "SceneConstants ABI mismatch.");
    static_assert(offsetof(SceneConstantBuffer, environmentWorldToLight) == 880, "SceneConstants ABI mismatch.");
    static_assert(offsetof(SceneConstantBuffer, environmentTint) == 944, "SceneConstants ABI mismatch.");

    struct GpuTexture
    {
        ComPtr<ID3D12Resource> resource;
        ComPtr<ID3D12Resource> upload;
        std::wstring path;
        DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM;
        uint32_t width = 1;
        uint32_t height = 1;
        uint32_t mipLevels = 1;
        bool fallback = false;
    };

    struct AccelerationStructureBuffers
    {
        ComPtr<ID3D12Resource> scratch;
        ComPtr<ID3D12Resource> result;
        ComPtr<ID3D12Resource> instanceDesc;
    };

    struct ShaderTableInfo
    {
        ComPtr<ID3D12Resource> resource;
        UINT recordSize = 0;
        UINT recordCount = 0;
    };

    struct MaterialSnapshot
    {
        XMFLOAT4 baseColorFactor = XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f);
        XMFLOAT4 emissiveFactor = XMFLOAT4(0.0f, 0.0f, 0.0f, 0.0f);
        float roughnessFactor = 0.48f;
        float metallicFactor = 0.0f;
        float occlusionStrength = 1.0f;
        float normalStrength = 1.0f;
        float alphaCutoff = 0.33f;
        bool alphaMasked = false;
        bool packedOcclusionRoughnessMetallic = false;
        std::array<std::wstring, TextureSlotCount> textures;
        std::array<bool, TextureSlotCount> textureOverrideEnabled = {};
    };

    struct MaterialVariant
    {
        std::string name;
        int materialIndex = 0;
        std::wstring materialName;
        MaterialSnapshot snapshot;
    };

    struct MaterialPreset
    {
        std::string name;
        std::string category;
        std::wstring sourcePath;
        MaterialSnapshot snapshot;
    };

    struct MaterialUsage
    {
        uint32_t meshCount = 0;
        uint64_t triangleCount = 0;
    };

    struct FrameContext
    {
        ComPtr<ID3D12CommandAllocator> commandAllocator;
        ComPtr<ID3D12Resource> sceneConstantBuffer;
        ComPtr<ID3D12Resource> gpuTimestampReadback;
        ComPtr<ID3D12Resource> qualityCounterReadback;
        UINT8* mappedSceneConstants = nullptr;
        UINT64 fenceValue = 0;
        UINT64 submissionSerial = 0;
        bool gpuTimingPending = false;
        bool qualityCountersPending = false;
        bool benchmarkMetricsPending = false;
        std::uint32_t benchmarkFrameIndex = 0;
        lookdevpt::benchmark::MetricValues benchmarkMetrics;
    };

    struct GpuTimingSample
    {
        bool valid = false;
        double pipelineMs = -1.0;
        double pathTraceMs = -1.0;
        double restirMs = -1.0;
        double restirCandidateMs = -1.0;
        double restirTemporalMs = -1.0;
        double restirSpatialMs = -1.0;
        double restirShadeMs = -1.0;
        double restirPublishMs = -1.0;
        double restirGiInitialMs = -1.0;
        double restirGiFusedMs = -1.0;
        double restirPtInitialMs = -1.0;
        double restirPtFusedMs = -1.0;
        double denoiseMs = -1.0;
        double denoisePrepareMs = -1.0;
        double denoiseCoreMs = -1.0;
        double denoiseCompositeMs = -1.0;
        double finalTaaMs = -1.0;
        double qualityCountersMs = -1.0;
        double historyPublishMs = -1.0;
        double copyMs = -1.0;
        double uiMs = -1.0;
    };

    CD3DX12_VIEWPORT m_viewport;
    CD3DX12_RECT m_scissorRect;
    ComPtr<IDXGISwapChain3> m_swapChain;
    ComPtr<ID3D12Device5> m_device;
    ComPtr<ID3D12GraphicsCommandList4> m_commandList;
    ComPtr<ID3D12CommandQueue> m_commandQueue;
    std::array<FrameContext, FrameCount> m_frameContexts;
    ComPtr<ID3D12DescriptorHeap> m_rtvHeap;
    ComPtr<ID3D12DescriptorHeap> m_descriptorHeap;
    ComPtr<ID3D12RootSignature> m_globalRootSignature;
    ComPtr<ID3D12StateObject> m_stateObject;
    ComPtr<ID3D12StateObjectProperties> m_stateObjectProperties;
    ComPtr<ID3D12CommandSignature> m_dispatchRaysCommandSignature;
    // RTXDI DI is a two-dispatch graph: candidate+temporal Pass A and
    // spatial+visibility+shade Pass B.
    ComPtr<ID3D12PipelineState> m_rtxdiDiCandidatePipeline;
    ComPtr<ID3D12PipelineState> m_rtxdiDiSpatialPipeline;
    ComPtr<ID3D12PipelineState> m_rtxdiGiInitialPipeline;
    ComPtr<ID3D12PipelineState> m_rtxdiGiFusedPipeline;
    ComPtr<ID3D12PipelineState> m_rtxdiPtInitialPipeline;
    ComPtr<ID3D12PipelineState> m_rtxdiPtFusedPipeline;
    ComPtr<ID3D12PipelineState> m_denoiseTemporalPipeline;
    std::array<ComPtr<ID3D12PipelineState>, DenoiseAtrousPipelineCount> m_denoiseAtrousPipelines;
    ComPtr<ID3D12PipelineState> m_denoiseCompositePipeline;
    ComPtr<ID3D12PipelineState> m_nrdPreparePipeline;
    ComPtr<ID3D12PipelineState> m_nrdCompositePipeline;
    ComPtr<ID3D12PipelineState> m_dlssPreparePipeline;
    ComPtr<ID3D12PipelineState> m_secondaryTaskCountPipeline;
    ComPtr<ID3D12PipelineState> m_secondaryGroupScanPipeline;
    ComPtr<ID3D12PipelineState> m_secondaryTaskScatterPipeline;
    ComPtr<ID3D12PipelineState> m_secondaryResolvePipeline;
    ComPtr<ID3D12PipelineState> m_finalTaaPipeline;
    ComPtr<ID3D12PipelineState> m_qualityCounterPipeline;
    ComPtr<ID3D12Resource> m_renderTargets[FrameCount];
    ComPtr<ID3D12Resource> m_PathtracingOutput;
    ComPtr<ID3D12Resource> m_accumulationOutput;
    ComPtr<ID3D12Resource> m_denoiseAov0;
    ComPtr<ID3D12Resource> m_denoiseAov1;
    ComPtr<ID3D12Resource> m_denoiseAov2;
    ComPtr<ID3D12Resource> m_reconstructionHistoryRadiance;
    ComPtr<ID3D12Resource> m_reconstructionHistoryMoments;
    ComPtr<ID3D12Resource> m_reconstructionHistoryLength;
    ComPtr<ID3D12Resource> m_reconstructionHistoryRadianceB;
    ComPtr<ID3D12Resource> m_reconstructionHistoryMomentsB;
    ComPtr<ID3D12Resource> m_reconstructionHistoryLengthB;
    ComPtr<ID3D12Resource> m_previousDenoiseAov0;
    ComPtr<ID3D12Resource> m_previousDenoiseAov1;
    ComPtr<ID3D12Resource> m_previousDenoiseAov2;
    ComPtr<ID3D12Resource> m_surfaceIdentity;
    ComPtr<ID3D12Resource> m_previousSurfaceIdentity;
    // Two physical reservoirs. Current/history alias fixed storage A; spatial
    // is Pass-A scratch B. This avoids per-frame descriptor mutation/copies.
    ComPtr<ID3D12Resource> m_restirReservoirCurrent;
    ComPtr<ID3D12Resource> m_restirReservoirHistory;
    ComPtr<ID3D12Resource> m_restirReservoirSpatial;
    ComPtr<ID3D12Resource> m_restirReservoirSpatialB;
    ComPtr<ID3D12Resource> m_restirDiReservoirCurrent;
    ComPtr<ID3D12Resource> m_restirDiReservoirHistory;
    ComPtr<ID3D12Resource> m_restirDiReservoirSpatial;
    ComPtr<ID3D12Resource> m_restirGiReservoirA;
    ComPtr<ID3D12Resource> m_restirGiReservoirB;
    ComPtr<ID3D12Resource> m_restirPtReservoirA;
    ComPtr<ID3D12Resource> m_restirPtReservoirB;
    std::array<ComPtr<ID3D12Heap>, 2> m_restirAliasHeaps;
    UINT64 m_restirAliasHeapSize = 0;
    ComPtr<ID3D12Resource> m_signalCurrentRadiance;
    ComPtr<ID3D12Resource> m_signalDirect;
    ComPtr<ID3D12Resource> m_signalIndirect;
    ComPtr<ID3D12Resource> m_signalResidual;
    ComPtr<ID3D12Resource> m_denoisePing;
    ComPtr<ID3D12Resource> m_denoisePong;
    ComPtr<ID3D12Resource> m_nrdMotion;
    ComPtr<ID3D12Resource> m_nrdNormalRoughness;
    ComPtr<ID3D12Resource> m_nrdViewZ;
    ComPtr<ID3D12Resource> m_nrdDiffRadianceHitDistance;
    ComPtr<ID3D12Resource> m_nrdSpecRadianceHitDistance;
    ComPtr<ID3D12Resource> m_nrdDiffDenoised;
    ComPtr<ID3D12Resource> m_nrdSpecDenoised;
    ComPtr<ID3D12Resource> m_postDenoiseHdr;
    bool m_fuseNrdFinalTaaForFrame = false;
    ComPtr<ID3D12Resource> m_taaHistoryA;
    ComPtr<ID3D12Resource> m_taaHistoryB;
    ComPtr<ID3D12Resource> m_finalResolvedHdr;
    bool m_accumulationAliasesTaaHistory = false;
    ComPtr<ID3D12Resource> m_qualityCounterBuffer;
    ComPtr<ID3D12Resource> m_qualityContribution;
    ComPtr<ID3D12Resource> m_nrdDiffuseConfidence;
    ComPtr<ID3D12Resource> m_nrdSpecularConfidence;
    ComPtr<ID3D12Resource> m_dlssDepth;
    ComPtr<ID3D12Resource> m_dlssMotion;
    ComPtr<ID3D12Resource> m_dlssNormalRoughness;
    ComPtr<ID3D12Resource> m_dlssAlbedo;
    ComPtr<ID3D12Resource> m_dlssSpecularAlbedo;
    ComPtr<ID3D12Resource> m_dlssExposure;
    ComPtr<ID3D12Resource> m_primaryPositionCone;
    ComPtr<ID3D12Resource> m_primaryGeometricNormal;
    ComPtr<ID3D12Resource> m_primaryIdentity;
    ComPtr<ID3D12Resource> m_secondaryTaskOffsets;
    ComPtr<ID3D12Resource> m_secondaryGroupOffsets;
    ComPtr<ID3D12Resource> m_secondaryTasks;
    ComPtr<ID3D12Resource> m_secondaryResults;
    ComPtr<ID3D12Resource> m_secondaryIndirectArgs;
    ComPtr<ID3D12Resource> m_reviewProbeBuffer;
    UINT m_secondaryTaskCapacity = 1;
    UINT m_secondaryGroupCount = 1;
    bool m_dlssFallbackRebuildAfterFrame = false;
    ComPtr<ID3D12QueryHeap> m_gpuTimestampQueryHeap;
    UINT m_frameIndex = 0;
    UINT m_rtvDescriptorSize = 0;
    UINT m_descriptorSize = 0;
    UINT m_descriptorCount = 0;
    UINT m_alternateOutputTableBase = 0;
    UINT m_restirReservoirElementCount = 1;
    UINT64 m_restirReservoirBufferSize = 0;
    UINT m_restirGiReservoirElementCount = 1;
    UINT64 m_restirGiReservoirBufferSize = 0;
    UINT m_restirPtReservoirElementCount = 1;
    UINT64 m_restirPtReservoirBufferSize = 0;
    UINT m_qualityCounterTileCountX = 1;
    UINT m_qualityCounterTileCountY = 1;
    UINT m_qualityCounterTileCount = 1;
    UINT64 m_qualityCounterBufferSize = sizeof(lookdevpt::benchmark::QualityCounterTileV1);
    UINT64 m_frameHistoryResourceBytes = 0;
    UINT64 m_gpuTimestampFrequency = 0;
    bool m_gpuTimingSupported = false;
    bool m_gpuTimingValid = false;
    double m_gpuFrameMs = 0.0;
    double m_gpuPathTraceMs = 0.0;
    double m_gpuRestirMs = 0.0;
    double m_gpuRestirCandidateMs = 0.0;
    double m_gpuRestirTemporalMs = 0.0;
    double m_gpuRestirSpatialMs = 0.0;
    double m_gpuRestirShadeMs = 0.0;
    double m_gpuRestirPublishMs = 0.0;
    double m_gpuRestirGiInitialMs = 0.0;
    double m_gpuRestirGiFusedMs = 0.0;
    double m_gpuRestirPtInitialMs = 0.0;
    double m_gpuRestirPtFusedMs = 0.0;
    double m_gpuDenoiseMs = 0.0;
    double m_gpuDenoisePrepareMs = 0.0;
    double m_gpuDenoiseCoreMs = 0.0;
    double m_gpuDenoiseCompositeMs = 0.0;
    double m_gpuFinalTaaMs = 0.0;
    double m_gpuQualityCountersMs = 0.0;
    double m_gpuHistoryPublishMs = 0.0;
    double m_gpuCopyMs = 0.0;
    double m_gpuUiMs = 0.0;
    UINT64 m_nextSubmissionSerial = 1;
    UINT64 m_gpuTimingFrameSerial = 0;

    PathTracingMode m_mode = PathTracingMode::Pathtracing;
    Bistro::Scene m_scene;
    std::vector<Bistro::RtGeometryRecord> m_geometryRecords;
    std::vector<Bistro::RtMaterial> m_rtMaterials;
    std::vector<Bistro::RtInstance> m_rtInstances;
    std::vector<GpuTexture> m_textures;
    std::vector<std::array<UINT, Bistro::TextureSlotCount>> m_materialTextureIndices;
    std::vector<std::array<bool, Bistro::TextureSlotCount>> m_materialTextureExists;
    ComPtr<ID3D12Resource> m_vertexBuffer;
    ComPtr<ID3D12Resource> m_indexBuffer;
    ComPtr<ID3D12Resource> m_geometryBuffer;
    ComPtr<ID3D12Resource> m_materialBuffer;
    ComPtr<ID3D12Resource> m_lightBuffer;
    ComPtr<ID3D12Resource> m_instanceBuffer;
    std::vector<ComPtr<ID3D12Resource>> m_uploadBuffers;
    AccelerationStructureBuffers m_bottomLevelAs;
    std::vector<AccelerationStructureBuffers> m_bottomLevelInstances;
    AccelerationStructureBuffers m_topLevelAs;
    UINT64 m_blasOriginalBytes = 0;
    UINT64 m_blasCompactedBytes = 0;
    ShaderTableInfo m_rayGenTable;
    ShaderTableInfo m_primaryRayGenTable;
    ShaderTableInfo m_secondaryRayGenTable;
    ShaderTableInfo m_reviewProbeRayGenTable;
    ShaderTableInfo m_missTable;
    ShaderTableInfo m_hitGroupTable;

    Bistro::FpsCamera m_camera;
    XMFLOAT3 m_defaultCameraPosition = XMFLOAT3(-16.32f, 4.66f, -10.41f);
    float m_defaultCameraYaw = DirectX::XMConvertToRadians(18.1f);
    float m_defaultCameraPitch = DirectX::XMConvertToRadians(2.8f);
    float m_defaultCameraRoll = 0.0f;
    float m_cameraFovDegrees = 60.0f;
    float m_defaultCameraFovDegrees = 60.0f;
    std::chrono::steady_clock::time_point m_lastUpdate;
    float m_lightDirection[3] = { -0.35f, -0.8f, 0.45f };
    float m_lightColor[3] = { 1.0f, 0.96f, 0.88f };
    float m_lightIntensity = 4.0f;
    float m_skyColor[3] = { 0.015f, 0.08f, 0.16f };
    float m_skyHorizonColor[3] = { 0.42f, 0.63f, 0.86f };
    float m_skyZenithColor[3] = { 0.05f, 0.20f, 0.52f };
    float m_skyGroundColor[3] = { 0.025f, 0.035f, 0.045f };
    float m_skyIntensity = 1.0f;
    float m_sunIntensity = 8.0f;
    float m_sunAngularRadius = 0.012f;
    float m_skyGroundBlend = 0.35f;
    float m_emissiveLightIntensity = 4.0f;
    float m_proceduralLightIntensity = 12.0f;
    float m_environmentIntensity = 1.0f;
    float m_environmentRotation = 0.0f;
    float m_rayTMin = 0.03f;
    float m_rayTMax = 10000.0f;
    float m_giStrength = 0.6f;
    int m_giSamplesPerFrame = 2;
    float m_giRadianceClamp = 8.0f;
    float m_giTemporalClampScale = 1.5f;
    float m_giTemporalClampMin = 0.25f;
    int m_maxPathBounces = 4;
    int m_minPathBounces = 2;
    int m_maxAccumulatedFrames = 256;
    uint32_t m_accumulatedFrames = 0;
    uint32_t m_frameCounter = 0;
    bool m_freezeAccumulation = false;
    bool m_resetAccumulationRequested = true;
    rb::FrameState m_frameState;
    rb::FrameChangeMask m_pendingFrameChanges = rb::FrameChangeMask::None;
    rb::HistoryDomain m_validHistoryDomains = rb::HistoryDomain::None;
    float m_baseMoveSpeed = 17.0f;
    float m_fastMoveSpeed = 58.2f;
    int m_debugViewMode = 0;
    bool m_debugNormalMapYFlip = true;
    bool m_shadowEnabled = true;
    bool m_skyNeeEnabled = true;
    bool m_emissiveLightsEnabled = true;
    bool m_proceduralLightsEnabled = true;
    bool m_environmentMapEnabled = false;
    bool m_environmentEqualAreaMapping = false;
    bool m_restirTemporalReuse = true;
    int m_restirSpatialReusePasses = 2;
    int m_restirSpatialRadius = 16;
    int m_restirCandidateSamples = 1;
    float m_restirMClamp = 20.0f;
    bool m_restirDiTemporalReuse = true;
    int m_restirDiSpatialReusePasses = 2;
    int m_restirDiCandidateSamples = 1;
    float m_restirDiMClamp = 20.0f;
    bool m_denoiserEnabled = true;
    DenoiseBackend m_denoiseBackend = DenoiseBackend::Internal;
    DlssMode m_dlssMode = DlssMode::Quality;
    bool m_dlssEnabledWhenAvailable = false;
    int m_denoiserSpatialIterations = 2;
    float m_denoiserNormalSigma = 0.25f;
    float m_denoiserDepthSigma = 0.02f;
    float m_denoiserLuminanceSigma = 1.5f;
    float m_denoiserAlbedoSigma = 0.35f;
    float m_denoiserStrength = 0.85f;
    bool m_splitSignalDenoise = true;
    NoisePreset m_noisePreset = NoisePreset::InteractiveStable;
    bool m_temporalStabilityEnabled = true;
    float m_historyClampSigma = 1.5f;
    float m_reactiveThreshold = 0.35f;
    float m_specularHistoryScale = 0.45f;
    bool m_realtimeReconstruction = true;
    bool m_cameraJitter = true;
    JitterMode m_jitterMode = JitterMode::Stable32;
    float m_movingJitterScale = 0.25f;
    int m_reconstructionMaxHistoryFrames = 32;
    float m_temporalAlphaMin = 0.04f;
    float m_temporalAlphaMax = 0.22f;
    float m_validationNormalDotThreshold = 0.82f;
    float m_validationDepthRelativeThreshold = 0.035f;
    float m_validationAlbedoThreshold = 0.55f;
    float m_validationRoughnessThreshold = 0.35f;
    int m_atrousPassCount = 3;
    float m_atrousDiffuseStrength = 0.85f;
    float m_atrousSpecularStrength = 0.35f;
    float m_atrousVarianceScale = 1.25f;
    bool m_adaptiveSamplingEnabled = true;
    int m_maxAdaptiveSamplesPerPixel = 2;
    float m_adaptiveVarianceThreshold = 0.18f;
    float m_adaptiveDisocclusionBoost = 1.0f;
    bool m_reservoirReprojection = true;
    bool m_reservoirValidation = true;
    bool m_restirGiValidationRay = false;
    int m_reservoirMaxAge = 8;
    bool m_skyEnabled = true;
    bool m_vsyncEnabled = false;
    bool m_tearingSupported = false;
    bool m_viewportFocused = false;
    HANDLE m_frameLatencyWaitableObject = nullptr;
    D3D12_RAYTRACING_TIER m_raytracingTier = D3D12_RAYTRACING_TIER_NOT_SUPPORTED;
    XMFLOAT4X4 m_previousViewProjection =
    {
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f
    };
    XMFLOAT4X4 m_nrdViewToClip =
    {
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f
    };
    XMFLOAT4X4 m_nrdViewToClipPrev =
    {
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f
    };
    XMFLOAT4X4 m_nrdWorldToView =
    {
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f
    };
    XMFLOAT4X4 m_nrdWorldToViewPrev =
    {
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f
    };
    bool m_hasPreviousViewProjection = false;
    XMFLOAT2 m_currentJitter = XMFLOAT2(0.0f, 0.0f);
    XMFLOAT2 m_previousJitter = XMFLOAT2(0.0f, 0.0f);
    XMFLOAT2 m_nrdCurrentJitterForFrame = XMFLOAT2(0.0f, 0.0f);
    XMFLOAT2 m_nrdPreviousJitterForFrame = XMFLOAT2(0.0f, 0.0f);
    float m_currentJitterStrength = 1.0f;
    float m_cameraMotionAmount = 0.0f;
    uint32_t m_framesSinceCameraMotion = 0;
    bool m_cameraMotionTrackingInitialized = false;
    bool m_forcePreserveCameraHistoryOnce = false;
    bool m_denoiseHistoryValid = false;
    bool m_resetDenoiseHistoryRequested = true;
    XMFLOAT4 m_previousCameraMotionState = XMFLOAT4(0, 0, 0, 0);
    float m_previousCameraMotionPitch = 0.0f;
    XMFLOAT2 m_previousCameraMotionRollFov = XMFLOAT2(0.0f, 60.0f);
    XMFLOAT4 m_lastCameraAndYaw = XMFLOAT4(0, 0, 0, 0);
    float m_lastCameraPitch = 0.0f;
    XMFLOAT2 m_lastCameraRollFov = XMFLOAT2(0.0f, 60.0f);
    XMFLOAT4 m_lastLighting = XMFLOAT4(0, 0, 0, 0);
    XMFLOAT4 m_lastGiOptions = XMFLOAT4(0, 0, 0, 0);
    XMFLOAT4 m_lastPathOptions = XMFLOAT4(0, 0, 0, 0);
    XMFLOAT4 m_lastRestirOptions = XMFLOAT4(0, 0, 0, 0);
    XMFLOAT4 m_lastRestirDiOptions = XMFLOAT4(0, 0, 0, 0);
    XMFLOAT4 m_lastLightSystemOptions = XMFLOAT4(0, 0, 0, 0);
    XMFLOAT4 m_lastSignalDenoiseOptions = XMFLOAT4(0, 0, 0, 0);
    XMFLOAT4 m_lastViewOptions = XMFLOAT4(0, 0, 0, 0);
    rb::QualitySettings m_qualitySettings;
    uint32_t m_overBudgetFrameCount = 0;
    uint32_t m_underBudgetFrameCount = 0;
    int m_rayBudgetBouncePenalty = 0;
    bool m_rayBudgetExtraSampleEnabled = true;
    bool m_autoSecondaryHalfActive = false;
    float m_activeSecondaryShadingRate = 1.0f;
    rb::RayBudgetState m_rayBudgetState;
    bool m_rtxdiAvailable = false;
    std::wstring m_environmentTexturePath;
    XMFLOAT4X4 m_environmentLightToWorld = {
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f };
    XMFLOAT4X4 m_environmentWorldToLight = {
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f };
    XMFLOAT3 m_environmentTint = XMFLOAT3(1.0f, 1.0f, 1.0f);
    UINT m_environmentDescriptorIndex = 0;
    uint32_t m_activeLightCount = 0;
    uint32_t m_emissiveTriangleLightCount = 0;
    uint32_t m_proceduralAreaLightCount = 0;
    std::vector<Bistro::RtLight> m_lights;
    std::vector<Bistro::Material> m_sourceMaterials;
    std::vector<std::array<bool, TextureSlotCount>> m_textureOverrideEnabled;
    std::vector<MaterialUsage> m_materialUsage;
    uint64_t m_sceneSubmittedIndexCount = 0;
    uint64_t m_scenePrimitiveCount = 0;
    std::vector<MaterialVariant> m_materialVariants;
    std::vector<MaterialPreset> m_materialPresets;
    MaterialSnapshot m_materialCompareA;
    MaterialSnapshot m_materialCompareB;
    bool m_hasMaterialCompareA = false;
    bool m_hasMaterialCompareB = false;
    int m_selectedMaterial = 0;
    char m_materialSearch[128] = {};
    char m_materialVariantName[128] = "Variant";
    char m_materialPresetName[128] = "Preset";
    ToneMapper m_toneMapper = ToneMapper::Aces;
    float m_exposure = 0.0f;
    float m_gamma = 2.2f;
    MaterialFocusMode m_materialFocusMode = MaterialFocusMode::Normal;

    HANDLE m_fenceEvent = nullptr;
    ComPtr<ID3D12Fence> m_fence;
    UINT64 m_fenceValue = 0;
    ComPtr<ID3D12CommandAllocator> m_mcpReviewCommandAllocator;
    ComPtr<ID3D12GraphicsCommandList> m_mcpReviewCommandList;

    void LoadPipeline();
    void LoadAssets();
    void CreateGpuResourcesForCurrentScene();
    void RefreshEditableGpuResources(bool reloadTextures, bool rebuildLights);
    void RequestGpuResourceRefresh(PendingGpuResourceRefresh refresh);
    void RebuildLightList();
    void CreateLightBuffer();
    void CreateMaterialBuffer();
    bool LoadScenePath(const std::wstring& path, std::string& diagnostics);
    bool CommitImportedScene(const std::wstring& path, rb::SceneImportResult&& imported, std::string& diagnostics);
    void BeginAsyncSceneLoad(const std::wstring& path);
    void PollAsyncSceneLoad();
    void CancelAsyncSceneLoad();
    bool LoadEnvironmentPath(const std::wstring& path, std::string& diagnostics);
    bool SaveProjectToDisk(const std::wstring& path);
    bool LoadProjectFromDisk(const std::wstring& path, std::string& diagnostics);
    void LoadStartupSettings();
    void ApplyStartupSettings();
    bool SaveStartupSettingsToDisk();
    bool DeleteStartupSettings();
    bool ApplyAction(const std::string& method, const cld::JsonValue& params, std::string& diagnostics, bool validateOnly);
    mcp::ToolResult CallMcpTool(const std::string& name, const cld::JsonValue& arguments, int timeoutMs) override;
    std::optional<mcp::ToolResult> CallMcpReviewTool(
        const std::string& name,
        const cld::JsonValue& arguments,
        int timeoutMs);
    mcp::ResourceResult ReadMcpResource(const std::string& uri) override;
    size_t PendingMcpCommandCount() const override;
    void LoadMcpUserSettings();
    void SaveMcpUserSettings();
    void StartMcpServer();
    void StopMcpServer();
    void ProcessMcpCommands();
    void UpdateMcpSnapshots();
    std::string BuildMcpStateJson() const;
    std::string BuildMcpStatsJson() const;
    std::string BuildMcpMaterialsJson() const;
    mcp::ToolResult SubmitMcpActionTool(const std::string& toolName, const std::string& actionMethod, const cld::JsonValue& params, bool validateOnly, int timeoutMs);
    mcp::ToolResult MakeMcpJsonToolResult(bool ok, const std::string& text, const std::string& structuredJson) const;
    bool CaptureViewportPng(std::string& base64Png, std::string& diagnostics);
    bool CaptureViewportPng(
        std::string& base64Png,
        std::string& diagnostics,
        const std::filesystem::path& outputPath,
        lookdevpt::benchmark::ArtifactStatistics* statistics = nullptr);
    bool BeginMcpReviewCapture(uint64_t reviewId, int debugView, std::string& diagnostics);
    void PollMcpReviewCapture();
    bool CaptureTextureArtifact(
        ID3D12Resource* texture,
        const std::filesystem::path& outputPath,
        bool radianceHdr,
        std::string& diagnostics,
        lookdevpt::benchmark::ArtifactStatistics* statistics = nullptr);
    bool SaveBenchmarkArtifactSet(
        const std::filesystem::path& relativeDirectory,
        const std::filesystem::path& ldrFileName,
        const std::filesystem::path& hdrFileName,
        lookdevpt::benchmark::ArtifactPhase phase,
        std::uint32_t frameIndex,
        bool includeAovs,
        std::string& diagnostics);
    bool SaveBenchmarkArtifacts(std::string& diagnostics);
    bool SaveBenchmarkFrameArtifacts(std::uint32_t frameIndex, std::string& diagnostics);
    void StageBenchmarkFrameForSubmission(
        UINT frameContextIndex,
        UINT64 submissionSerial,
        double cpuFrameMs,
        double cpuFenceWaitMs);
    void CompleteBenchmarkFrame(FrameContext& frameContext, const GpuTimingSample& timing);
    void DrainCompletedBenchmarkMetrics();
    void FinalizeBenchmarkRun();
    bool RenderPathTracingOutputForCapture(std::string& diagnostics);
    uint64_t StoreMcpCapture(std::string base64Png, int debugView, const std::string& label);
    bool FindMcpCapture(uint64_t id, std::string& base64Png, std::string& label) const;
    std::string BuildMcpDiagnosticsJson() const;
    std::string BuildMcpProjectJson() const;
    std::string BuildMcpSceneSummaryJson() const;
    std::string BuildMcpCaptureIndexJson() const;
    void RefreshMcpAuditCache();
    void ProcessMcpReview();
    void ProcessMcpBenchmark();
    std::string BuildMcpReviewIndexJson() const;
    std::string BuildMcpReviewJson(uint64_t id) const;
    std::string BuildMcpCheckpointIndexJson() const;
    std::string BuildMcpBenchmarkIndexJson() const;
    std::string BuildMcpBenchmarkJson(uint64_t id) const;
    std::string SceneFingerprint() const;
    std::string CameraFingerprint() const;
    void EnforceMcpArtifactBudget();
    mcp::ToolResult SubmitMcpCommandTool(const std::string& toolName, const std::string& actionMethod, const cld::JsonValue& params, bool mutation, int timeoutMs);
    mcp::CommandResult ExecuteMcpCommand(const mcp::CommandRequest& request);
    void CreateDescriptorHeap();
    void CreateOutputResources();
    void RecreateResolutionDependentResources();
    void ApplyConfiguredRenderScale(bool resetDynamicScale);
    void UpdateDynamicResolution();
    bool UsesTemporalUpscale() const;
    bool UsesCompactSecondaryWorkList() const;
    void CreateGlobalRootSignature();
    void CreatePathtracingStateObject();
    void CreateSceneBuffers();
    void CreateTextures();
    void BuildAccelerationStructures();
    void BuildInstancedAccelerationStructures();
    void CreateShaderTables();
    void CreateRestirReusePipeline();
    void CreateDenoisePipeline();
    void CreateSecondaryWorkPipelines();
    void PopulateCommandList();
    void DispatchRays();
    void DispatchCompactSecondaryWork();
    void RunRestirReusePass();
    void RunRestirGiPass();
    void RunRestirPtPass();
    void RunDenoisePass();
    bool RunNrdDenoisePass();
    bool RunDlssRayReconstructionPass();
    void RunFinalTaaPass();
    bool ShouldFuseNrdFinalTaa() const;
    void RunQualityCounterPass();
    void SealSurfaceGuideFrame();
    void CopyOutputToBackBuffer();
    void CreateGpuTimingResources();
    void BeginGpuTimingFrame();
    void WriteGpuTimestamp(GpuTimestamp timestamp);
    void ResolveGpuTimingQueries();
    void ReadbackGpuTimingQueries(UINT frameIndex);
    void WaitForFrameContext(UINT frameIndex);
    UINT64 SignalFrameAndAdvance();
    void WaitForPreviousFrame();
    void UpdateConstantBuffer(float deltaSeconds);
    void ResetLight();
    void ResetCameraView();
    void ResetCameraSpeeds();
    void ResetAccumulation();
    void ResetDenoiseHistory();
    void ResetRenderingHistory();
    void InvalidateHistory(rb::FrameChangeMask changes);
    void InvalidateHistoryDomains(rb::HistoryDomain domains, rb::FrameChangeMask changes);
    void ApplyNoisePreset(NoisePreset preset);
    bool ShouldRunInternalDenoiser() const;
    bool IsDlssSelected() const;
    bool IsNrdSelected() const;
    NrdMethod SelectedNrdMethod() const;
    const char* ActiveDenoiseBackendName() const;
    const char* ActiveDenoiseBackendDisplayName() const;
    std::string BuildDlssStatusJson() const;
    std::string BuildNrdStatusJson() const;
    void InitializeMaterialLookDevState(bool clearVariants);
    void RebuildMaterialUsage();
    int ResolveMaterialIndex(const cld::JsonValue& params) const;
    MaterialSnapshot CaptureMaterialSnapshot(int materialIndex) const;
    static bool RequiresMaterialTextureReload(const MaterialSnapshot& before, const MaterialSnapshot& after);
    static bool RequiresMaterialBlasRebuild(const MaterialSnapshot& before, const MaterialSnapshot& after);
    void ApplyMaterialSnapshot(int materialIndex, const MaterialSnapshot& snapshot, bool useSnapshotTextureFlags);
    void ResetMaterialToSource(int materialIndex);
    bool ValidateMaterialTexturePath(const std::wstring& path, std::string& diagnostics) const;
    bool TryParseTextureSlot(const cld::JsonValue& value, UINT& slot) const;
    bool ApplyMaterialTextureOverride(int materialIndex, UINT slot, const std::wstring& path, bool enableOverride, std::string& diagnostics);
    void LoadMaterialPresets();
    bool SaveUserMaterialPreset(const std::string& name, int materialIndex, std::string& diagnostics);
    bool ApplyMaterialPreset(int materialIndex, size_t presetIndex, std::string& diagnostics);
    std::string BuildMaterialTexturesJson(size_t materialIndex) const;
    std::string BuildMaterialVariantsJson() const;
    std::string BuildMaterialPresetsJson() const;
    void UpdateCameraMotionState();
    void UpdateRayBudget();
    void ApplyQualitySettingsToRenderer();
    bool HasAccumulationStateChanged();
    UINT CreateTextureResource(
        const std::wstring& path,
        bool srgb,
        const uint8_t fallback[4],
        std::map<std::wstring, UINT>& cache,
        float alphaCoverageCutoff = -1.0f,
        bool environmentRadiance = false);
    ComPtr<ID3D12Resource> CreateDefaultBuffer(const void* data, UINT64 size, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES finalState, const wchar_t* name);
    ComPtr<ID3D12Resource> CreateUploadBuffer(const void* data, UINT64 size, const wchar_t* name);
    ComPtr<ID3D12Resource> CreateUavBuffer(UINT64 size, D3D12_RESOURCE_STATES initialState, const wchar_t* name);
    UINT CurrentSurfaceGuideParity() const;
    UINT LastSubmittedSurfaceGuideParity() const;
    ID3D12Resource* AccumulationResource(UINT parity) const;
    ID3D12Resource* FinalResolvedHdrResource(UINT parity) const;
    ID3D12Resource* SurfaceGuideAovResource(UINT plane, UINT parity) const;
    ID3D12Resource* SurfaceGuideIdentityResource(UINT parity) const;
    D3D12_GPU_DESCRIPTOR_HANDLE CurrentOutputTableGpuDescriptor() const;
    D3D12_CPU_DESCRIPTOR_HANDLE CpuDescriptor(UINT index) const;
    D3D12_GPU_DESCRIPTOR_HANDLE GpuDescriptor(UINT index) const;
    std::wstring ShaderFileName() const;
    std::wstring DenoiseShaderFileName() const;
    UINT MaxTraceRecursionDepth() const;
    void CreateRenderTargetViews();
    void Resize(UINT width, UINT height);
    static UINT Align(UINT value, UINT alignment);

    rb::SceneImporter m_sceneImporter;
    std::wstring m_scenePath;
    std::wstring m_projectPath;
    std::wstring m_startupSettingsPath;
    std::wstring m_startupProjectPath;
    std::wstring m_startupScenePath;
    std::wstring m_startupEnvironmentPath;
    std::string m_startupMcpToken;
    UINT m_startupMcpPort = 0;
    mcp::AuthenticationMode m_startupMcpAuthenticationMode =
        mcp::AuthenticationMode::BearerToken;
    mcp::AccessMode m_startupMcpAccessMode = mcp::AccessMode::ConfirmMutations;
    bool m_startupMcpServer = false;
    bool m_startupMcpPairing = false;
    bool m_hasStartupSettingsPath = false;
    bool m_hasCommandLineProjectPath = false;
    bool m_hasCommandLineScenePath = false;
    bool m_hasCommandLineEnvironmentPath = false;
    bool m_hasStartupMcpAuthenticationMode = false;
    bool m_hasStartupMcpAccessMode = false;
    std::wstring m_pendingProjectPath;
    std::wstring m_pendingScenePath;
    std::wstring m_queuedSceneLoadPath;
    std::wstring m_pendingEnvironmentPath;
    std::string m_sceneDiagnostics = "Using built-in preview scene.";
    std::string m_projectDiagnostics;
    std::string m_startupDiagnostics;
    PendingFileDialog m_pendingFileDialog = PendingFileDialog::None;
    std::future<rb::SceneImportResult> m_sceneLoadFuture;
    std::optional<rb::SceneImportResult> m_sceneLoadCpuResult;
    std::atomic_bool m_sceneLoadCancelRequested = false;
    std::atomic<SceneLoadStage> m_sceneLoadStage = SceneLoadStage::Idle;
    std::atomic_uint64_t m_sceneLoadCompleted = 0;
    std::atomic_uint64_t m_sceneLoadTotal = 0;
    mutable std::mutex m_sceneLoadProgressMutex;
    std::wstring m_sceneLoadCurrentAsset;
    UINT m_pendingResizeWidth = 0;
    UINT m_pendingResizeHeight = 0;
    UINT m_renderWidth = 0;
    UINT m_renderHeight = 0;
    float m_activeRenderScale = 1.0f;
    uint32_t m_dynamicResolutionOverBudgetFrames = 0;
    uint32_t m_dynamicResolutionUnderBudgetFrames = 0;
    PendingGpuResourceRefresh m_pendingGpuResourceRefresh = PendingGpuResourceRefresh::None;
    bool m_resizePending = false;
    bool m_minimized = false;
    bool m_projectDirty = false;
    std::wstring m_adapterDescription = L"Unknown";
    DlssBackend m_dlssBackendRuntime;
    NrdBackend m_nrdBackendRuntime;
    RtxdiBackendRuntime m_rtxdiBackendRuntime;
    mcp::ServerSettings m_mcpSettings;
    mutable std::mutex m_mcpSettingsMutex;
    mcp::Server m_mcpServer;
    mcp::Dispatcher m_mcpDispatcher;
    mutable std::mutex m_mcpSnapshotMutex;
    std::string m_mcpStateJson = "{}";
    std::string m_mcpStatsJson = "{}";
    std::string m_mcpMaterialsJson = "{}";
    std::string m_mcpDiagnosticsJson = "{}";
    std::string m_mcpProjectJson = "{}";
    std::string m_mcpSceneSummaryJson = "{}";
    std::string m_mcpMaterialVariantsJson = "{}";
    std::string m_mcpMaterialPresetsJson = "{}";
    std::string m_mcpAuditJson = "{}";
    uint32_t m_mcpAuditInfoCount = 0;
    uint32_t m_mcpAuditWarningCount = 0;
    uint32_t m_mcpAuditErrorCount = 0;
    std::string m_mcpLatestProbesJson = "{\"probes\":[]}";
    lookdevpt::review::SceneAuditSummary m_mcpSceneAuditSummary;
    bool m_mcpSceneAuditFresh = false;
    rb::FrameRevisions m_mcpAuditRevisions{};
    std::chrono::steady_clock::time_point m_mcpNextStateSnapshot{};
    std::chrono::steady_clock::time_point m_mcpNextStatsSnapshot{};
    rb::FrameRevisions m_mcpSnapshotRevisions{};
    uint64_t m_mcpMaterialCatalogRevision = 0;
    uint64_t m_mcpSnapshotMaterialCatalogRevision = 0;
    bool m_mcpSnapshotProjectDirty = false;
    bool m_mcpStaticSnapshotsValid = false;
    std::string m_mcpLatestCaptureBase64;
    std::string m_mcpLastCaptureDiagnostics = "No capture has been requested.";
    struct McpCapture
    {
        uint64_t id = 0;
        int debugView = 0;
        std::string label;
        std::string base64Png;
        lookdevpt::review::Rgba8Image rgba;
        std::string sceneFingerprint;
        std::string cameraFingerprint;
        std::string materialFingerprint;
        std::string lightingFingerprint;
        std::string backendFingerprint;
        size_t artifactBytes = 0;
        uint64_t accessSerial = 0;
        uint32_t pinCount = 0;
    };
    std::deque<McpCapture> m_mcpCaptures;
    uint64_t m_nextMcpCaptureId = 1;
    uint64_t m_mcpArtifactAccessSerial = 1;
    struct McpComparison
    {
        uint64_t id = 0;
        uint64_t beforeCaptureId = 0;
        uint64_t afterCaptureId = 0;
        std::string json;
        std::string heatmapBase64;
        size_t artifactBytes = 0;
        uint64_t accessSerial = 0;
    };
    std::deque<McpComparison> m_mcpComparisons;
    uint64_t m_nextMcpComparisonId = 1;
    struct McpReview
    {
        uint64_t id = 0;
        std::string preset;
        std::string state = "queued";
        std::string stage = "queued";
        std::string errorCode;
        std::string diagnostics;
        std::vector<int> views;
        std::vector<uint64_t> captureIds;
        size_t nextView = 0;
        uint64_t baselineCaptureId = 0;
        uint64_t comparisonId = 0;
        uint64_t startFrame = 0;
        uint64_t settleUntilFrame = 0;
        std::chrono::steady_clock::time_point deadline{};
        double progress = 0.0;
        bool cancelRequested = false;
        bool artifactEvicted = false;
        std::string auditJson;
    };
    std::deque<McpReview> m_mcpReviews;
    uint64_t m_nextMcpReviewId = 1;
    uint64_t m_activeMcpReviewId = 0;
    struct McpCheckpoint
    {
        uint64_t id = 0;
        std::string label;
        std::string sceneFingerprint;
        std::string cameraFingerprint;
        std::filesystem::path snapshotPath;
        std::wstring projectPath;
        std::string projectDiagnostics;
        bool projectDirty = false;
    };
    std::deque<McpCheckpoint> m_mcpCheckpoints;
    uint64_t m_nextMcpCheckpointId = 1;
    struct McpBenchmark
    {
        uint64_t id = 0;
        std::string state = "queued";
        std::string kind = "combined";
        std::string diagnostics;
        std::filesystem::path outputDirectory;
        std::filesystem::path checkpointPath;
        std::wstring projectPath;
        std::string projectDiagnostics;
        bool projectDirty = false;
        bool vsyncEnabled = true;
        uint32_t samplingSeed = 0;
        uint32_t totalFrames = 0;
        uint32_t completedFrames = 0;
        uint32_t lastPublishedFrame = 0;
        bool cancelRequested = false;
    };
    std::deque<McpBenchmark> m_mcpBenchmarks;
    uint64_t m_nextMcpBenchmarkId = 1;
    uint64_t m_activeMcpBenchmarkId = 0;
    enum class McpReviewVisualization : uint8_t
    {
        Color,
        Radiance,
        Normal,
        ScalarX,
        ScalarY,
        ScalarZ,
        ScalarW,
        Motion,
        Variance,
    };
    struct PendingMcpReviewReadback
    {
        uint64_t reviewId = 0;
        int debugView = 0;
        McpReviewVisualization visualization = McpReviewVisualization::Color;
        ComPtr<ID3D12Resource> resource;
        D3D12_RESOURCE_DESC sourceDesc{};
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout{};
        UINT rows = 0;
        UINT64 rowBytes = 0;
        UINT64 totalBytes = 0;
        UINT64 fenceValue = 0;
        std::string sceneFingerprint;
        std::string cameraFingerprint;
        std::string materialFingerprint;
        std::string lightingFingerprint;
        std::string backendFingerprint;
    };
    struct McpReviewCaptureResult
    {
        uint64_t reviewId = 0;
        int debugView = 0;
        bool ok = false;
        std::string base64Png;
        lookdevpt::review::Rgba8Image rgba;
        std::string diagnostics;
        std::string sceneFingerprint;
        std::string cameraFingerprint;
        std::string materialFingerprint;
        std::string lightingFingerprint;
        std::string backendFingerprint;
    };
    std::optional<PendingMcpReviewReadback> m_pendingMcpReviewReadback;
    std::future<McpReviewCaptureResult> m_mcpReviewCaptureFuture;
    static McpReviewCaptureResult EncodeMcpReviewCapture(
        uint64_t reviewId,
        int debugView,
        McpReviewVisualization visualization,
        D3D12_RESOURCE_DESC sourceDesc,
        UINT64 rowBytes,
        std::vector<uint8_t> sourcePixels);
    lookdevpt::benchmark::Options m_benchmarkOptions;
    std::unique_ptr<lookdevpt::benchmark::Harness> m_benchmarkHarness;
    lookdevpt::benchmark::FramePlan m_benchmarkFramePlan;
    std::chrono::steady_clock::time_point m_benchmarkCpuFrameStart;
    double m_benchmarkCpuFenceWaitMs = 0.0;
    double m_benchmarkCpuUpdateMs = 0.0;
    double m_benchmarkCpuMcpMs = 0.0;
    double m_benchmarkCpuUiMs = 0.0;
    double m_pendingEditorCpuMs = 0.0;
    double m_benchmarkCpuCommandRecordingMs = 0.0;
    double m_benchmarkCpuPresentMs = 0.0;
    double m_benchmarkCpuNrdRecordingMs = 0.0;
    std::string m_benchmarkDiagnostics;
    uint32_t m_benchmarkFrameIndex = 0;
    uint32_t m_benchmarkRecordedFrameCount = 0;
    std::map<uint32_t, lookdevpt::benchmark::MetricValues> m_completedBenchmarkMetrics;
    uint32_t m_samplingSeed = 0;
    bool m_benchmarkCommandLineValid = true;
    bool m_benchmarkFinished = false;
    int m_displayResolutionPreset = 1;
    std::string m_mcpUiDiagnostics;
    bool m_renderOnlyMode = false;
};
