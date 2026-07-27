#pragma once

#include <d3d12.h>
#include <DirectXMath.h>
#include <wrl/client.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

enum class NrdMethod
{
    ReblurDiffuseSpecular,
    RelaxDiffuseSpecular,
};

struct NrdStatus
{
    bool compiled = false;
    bool headersAvailable = false;
    bool libraryAvailable = false;
    bool initialized = false;
    bool instanceCreated = false;
    bool evaluationReady = false;
    bool historyResetRequested = false;
    uint32_t versionMajor = 0;
    uint32_t versionMinor = 0;
    uint32_t versionBuild = 0;
    uint32_t resourceWidth = 0;
    uint32_t resourceHeight = 0;
    uint32_t supportedDenoiserCount = 0;
    uint32_t pipelineCount = 0;
    uint32_t permanentPoolSize = 0;
    uint32_t transientPoolSize = 0;
    uint32_t constantBufferMaxDataSize = 0;
    std::string selectedDenoiser;
    std::string normalEncoding;
    std::string roughnessEncoding;
    std::string lastError;
    std::string fallbackReason;
};

struct NrdTextureBinding
{
    ID3D12Resource* resource = nullptr;
};

struct NrdEvaluationDesc
{
    ID3D12GraphicsCommandList* commandList = nullptr;
    uint32_t resourceWidth = 0;
    uint32_t resourceHeight = 0;
    uint32_t frameIndex = 0;
    uint32_t frameContextIndex = 0;
    float denoisingRange = 10000.0f;
    DirectX::XMFLOAT2 cameraJitter = DirectX::XMFLOAT2(0.0f, 0.0f);
    DirectX::XMFLOAT2 previousCameraJitter = DirectX::XMFLOAT2(0.0f, 0.0f);
    DirectX::XMFLOAT4X4 viewToClip = {};
    DirectX::XMFLOAT4X4 viewToClipPrev = {};
    DirectX::XMFLOAT4X4 worldToView = {};
    DirectX::XMFLOAT4X4 worldToViewPrev = {};
    NrdTextureBinding motion;
    NrdTextureBinding normalRoughness;
    NrdTextureBinding viewZ;
    NrdTextureBinding diffuseRadianceHitDistance;
    NrdTextureBinding specularRadianceHitDistance;
    NrdTextureBinding diffuseHistoryConfidence;
    NrdTextureBinding specularHistoryConfidence;
    NrdTextureBinding diffuseOutput;
    NrdTextureBinding specularOutput;
};

class NrdBackend
{
public:
    NrdBackend();
    ~NrdBackend();

    NrdBackend(const NrdBackend&) = delete;
    NrdBackend& operator=(const NrdBackend&) = delete;

    void Initialize(ID3D12Device* device, uint32_t resourceWidth, uint32_t resourceHeight, NrdMethod method, uint32_t frameContextCount = 1);
    void Resize(ID3D12Device* device, uint32_t resourceWidth, uint32_t resourceHeight);
    void UpdateMethod(ID3D12Device* device, NrdMethod method);
    void ResetHistory();
    void Shutdown();
    bool Evaluate(const NrdEvaluationDesc& desc);

    const NrdStatus& Status() const { return m_status; }
    bool CanEvaluate() const { return m_status.evaluationReady; }

private:
    struct PoolTexture
    {
        Microsoft::WRL::ComPtr<ID3D12Resource> resource;
        D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    };

    struct DescriptorBindingCache
    {
        ID3D12Resource* resource = nullptr;
        DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
        uint32_t descriptorIndex = 0;
        bool isSrv = false;
    };

    struct DispatchDescriptorCache
    {
        uint32_t pipelineIndex = ~0u;
        bool valid = false;
        std::vector<DescriptorBindingCache> bindings;
    };

    bool InitializeBridge(ID3D12Device* device);
    void ReleaseBridgeResources();

    void* m_instance = nullptr;
    NrdMethod m_method = NrdMethod::ReblurDiffuseSpecular;
    uint32_t m_resourceWidth = 0;
    uint32_t m_resourceHeight = 0;
    uint32_t m_frameContextCount = 1;
    NrdStatus m_status;
    Microsoft::WRL::ComPtr<ID3D12Device> m_device;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_rootSignature;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_descriptorHeap;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_constantBuffer;
    std::vector<Microsoft::WRL::ComPtr<ID3D12PipelineState>> m_pipelines;
    std::vector<PoolTexture> m_permanentPool;
    std::vector<PoolTexture> m_transientPool;
    std::vector<std::vector<DispatchDescriptorCache>> m_descriptorCaches;
    std::vector<D3D12_RESOURCE_BARRIER> m_transitionBarriers;
    std::vector<D3D12_RESOURCE_BARRIER> m_uavBarriers;
    std::vector<D3D12_RESOURCE_BARRIER> m_backToUavBarriers;
    std::vector<ID3D12Resource*> m_dispatchUavResources;
    uint8_t* m_mappedConstantBuffer = nullptr;
    uint32_t m_descriptorSize = 0;
    uint32_t m_srvDescriptorCount = 1;
    uint32_t m_uavDescriptorCount = 1;
    uint32_t m_descriptorSetStride = 2;
    uint32_t m_descriptorSetCount = 1;
    uint64_t m_constantBufferStride = 256;
    uint64_t m_constantBufferFrameStride = 256;
    uint64_t m_constantBufferSize = 256;
    uint64_t m_constantBufferCursor = 0;
    D3D12_GPU_VIRTUAL_ADDRESS m_lastConstantBufferAddress = 0;
};
