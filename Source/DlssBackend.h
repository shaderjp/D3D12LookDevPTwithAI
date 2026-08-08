#pragma once

#include <d3d12.h>
#include <dxgi1_6.h>
#include <DirectXMath.h>

#include <cstdint>
#include <string>

enum class DlssMode
{
    Quality,
    Balanced,
    Performance,
    UltraPerformance,
};

struct DlssStatus
{
    bool compiled = false;
    bool runtimeAvailable = false;
    bool initialized = false;
    bool deviceRegistered = false;
    bool applicationIdentityConfigured = false;
    bool featureSupported = false;
    bool evaluationReady = false;
    bool lastEvaluationSucceeded = false;
    bool fallbackRebuildRequested = false;
    bool historyResetRequested = false;
    uint64_t successfulEvaluations = 0;
    uint64_t failedEvaluations = 0;
    int32_t lastResultCode = 0;
    uint32_t recommendedRenderWidth = 0;
    uint32_t recommendedRenderHeight = 0;
    std::wstring runtimePath;
    std::string lastError;
    std::string lastFailureStage;
    std::string fallbackReason;
};

struct DlssEvaluationDesc
{
    ID3D12GraphicsCommandList* commandList = nullptr;
    ID3D12Resource* color = nullptr;
    ID3D12Resource* output = nullptr;
    ID3D12Resource* linearDepth = nullptr;
    ID3D12Resource* motion = nullptr;
    ID3D12Resource* normalRoughness = nullptr;
    ID3D12Resource* albedo = nullptr;
    ID3D12Resource* specularAlbedo = nullptr;
    ID3D12Resource* exposure = nullptr;
    uint32_t renderWidth = 0;
    uint32_t renderHeight = 0;
    uint32_t outputWidth = 0;
    uint32_t outputHeight = 0;
    uint32_t frameIndex = 0;
    float cameraNear = 0.1f;
    float cameraFar = 10000.0f;
    float cameraFovRadians = DirectX::XMConvertToRadians(60.0f);
    DirectX::XMFLOAT2 jitter = DirectX::XMFLOAT2(0.0f, 0.0f);
    DirectX::XMFLOAT3 cameraPosition = DirectX::XMFLOAT3(0.0f, 0.0f, 0.0f);
    DirectX::XMFLOAT4X4 viewToClip = {};
    DirectX::XMFLOAT4X4 previousViewToClip = {};
    DirectX::XMFLOAT4X4 worldToView = {};
    DirectX::XMFLOAT4X4 previousWorldToView = {};
    bool reset = false;
};

class DlssBackend
{
public:
    DlssBackend();
    ~DlssBackend();

    DlssBackend(const DlssBackend&) = delete;
    DlssBackend& operator=(const DlssBackend&) = delete;

    void InitializeBeforeDevice(const std::wstring& executableDirectory);
    void SetD3DDevice(ID3D12Device* device, IDXGIAdapter1* adapter, uint32_t outputWidth, uint32_t outputHeight, DlssMode mode);
    void UpdateMode(uint32_t outputWidth, uint32_t outputHeight, DlssMode mode);
    void ResetHistory();
    bool EvaluateRayReconstruction(const DlssEvaluationDesc& desc);
    bool ConsumeFallbackRebuildRequest();
    void Shutdown();

    const DlssStatus& Status() const { return m_status; }
    bool CanEvaluateRayReconstruction() const { return m_status.evaluationReady; }

private:
    void* m_runtime = nullptr;
    uint32_t m_applicationId = 0;
    uint32_t m_outputWidth = 0;
    uint32_t m_outputHeight = 0;
    DlssMode m_mode = DlssMode::Quality;
    DlssStatus m_status;
};
