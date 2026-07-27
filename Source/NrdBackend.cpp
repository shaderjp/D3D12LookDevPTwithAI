#include "stdafx.h"
#include "NrdBackend.h"

#include <algorithm>
#include <cstring>
#include <sstream>

#ifndef D3D12LOOKDEVPT_WITH_NRD
#define D3D12LOOKDEVPT_WITH_NRD 0
#endif

#if D3D12LOOKDEVPT_WITH_NRD
#include <NRD.h>
#endif

using Microsoft::WRL::ComPtr;

namespace
{
    constexpr uint32_t DenoiserIdentifier = 1;

    uint64_t AlignUp(uint64_t value, uint64_t alignment)
    {
        return (value + alignment - 1) & ~(alignment - 1);
    }

    std::wstring IndexedName(const wchar_t* prefix, uint32_t index)
    {
        return std::wstring(prefix) + L" " + std::to_wstring(index);
    }

#if D3D12LOOKDEVPT_WITH_NRD
    const char* ResultName(nrd::Result result)
    {
        switch (result)
        {
        case nrd::Result::SUCCESS:
            return "SUCCESS";
        case nrd::Result::FAILURE:
            return "FAILURE";
        case nrd::Result::INVALID_ARGUMENT:
            return "INVALID_ARGUMENT";
        case nrd::Result::UNSUPPORTED:
            return "UNSUPPORTED";
        case nrd::Result::NON_UNIQUE_IDENTIFIER:
            return "NON_UNIQUE_IDENTIFIER";
        default:
            return "UNKNOWN";
        }
    }

    const char* NormalEncodingName(nrd::NormalEncoding encoding)
    {
        switch (encoding)
        {
        case nrd::NormalEncoding::RGBA8_UNORM:
            return "RGBA8_UNORM";
        case nrd::NormalEncoding::RGBA8_SNORM:
            return "RGBA8_SNORM";
        case nrd::NormalEncoding::R10_G10_B10_A2_UNORM:
            return "R10_G10_B10_A2_UNORM";
        case nrd::NormalEncoding::RGBA16_UNORM:
            return "RGBA16_UNORM";
        case nrd::NormalEncoding::RGBA16_SNORM:
            return "RGBA16_SNORM";
        default:
            return "UNKNOWN";
        }
    }

    const char* RoughnessEncodingName(nrd::RoughnessEncoding encoding)
    {
        switch (encoding)
        {
        case nrd::RoughnessEncoding::SQ_LINEAR:
            return "SQ_LINEAR";
        case nrd::RoughnessEncoding::LINEAR:
            return "LINEAR";
        case nrd::RoughnessEncoding::SQRT_LINEAR:
            return "SQRT_LINEAR";
        default:
            return "UNKNOWN";
        }
    }

    nrd::Denoiser ToNrdDenoiser(NrdMethod method)
    {
        return method == NrdMethod::RelaxDiffuseSpecular
            ? nrd::Denoiser::RELAX_DIFFUSE_SPECULAR
            : nrd::Denoiser::REBLUR_DIFFUSE_SPECULAR;
    }

    const char* NrdDenoiserName(NrdMethod method)
    {
        return method == NrdMethod::RelaxDiffuseSpecular
            ? "RELAX_DIFFUSE_SPECULAR"
            : "REBLUR_DIFFUSE_SPECULAR";
    }

    bool IsSupported(const nrd::LibraryDesc& desc, nrd::Denoiser denoiser)
    {
        for (uint32_t i = 0; i < desc.supportedDenoisersNum; ++i)
        {
            if (desc.supportedDenoisers[i] == denoiser)
            {
                return true;
            }
        }
        return false;
    }

    DXGI_FORMAT ToDxgiFormat(nrd::Format format)
    {
        switch (format)
        {
        case nrd::Format::R8_UNORM:
            return DXGI_FORMAT_R8_UNORM;
        case nrd::Format::R8_SNORM:
            return DXGI_FORMAT_R8_SNORM;
        case nrd::Format::R8_UINT:
            return DXGI_FORMAT_R8_UINT;
        case nrd::Format::R8_SINT:
            return DXGI_FORMAT_R8_SINT;
        case nrd::Format::RG8_UNORM:
            return DXGI_FORMAT_R8G8_UNORM;
        case nrd::Format::RG8_SNORM:
            return DXGI_FORMAT_R8G8_SNORM;
        case nrd::Format::RG8_UINT:
            return DXGI_FORMAT_R8G8_UINT;
        case nrd::Format::RG8_SINT:
            return DXGI_FORMAT_R8G8_SINT;
        case nrd::Format::RGBA8_UNORM:
            return DXGI_FORMAT_R8G8B8A8_UNORM;
        case nrd::Format::RGBA8_SNORM:
            return DXGI_FORMAT_R8G8B8A8_SNORM;
        case nrd::Format::RGBA8_UINT:
            return DXGI_FORMAT_R8G8B8A8_UINT;
        case nrd::Format::RGBA8_SINT:
            return DXGI_FORMAT_R8G8B8A8_SINT;
        case nrd::Format::RGBA8_SRGB:
            return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        case nrd::Format::R16_UNORM:
            return DXGI_FORMAT_R16_UNORM;
        case nrd::Format::R16_SNORM:
            return DXGI_FORMAT_R16_SNORM;
        case nrd::Format::R16_UINT:
            return DXGI_FORMAT_R16_UINT;
        case nrd::Format::R16_SINT:
            return DXGI_FORMAT_R16_SINT;
        case nrd::Format::R16_SFLOAT:
            return DXGI_FORMAT_R16_FLOAT;
        case nrd::Format::RG16_UNORM:
            return DXGI_FORMAT_R16G16_UNORM;
        case nrd::Format::RG16_SNORM:
            return DXGI_FORMAT_R16G16_SNORM;
        case nrd::Format::RG16_UINT:
            return DXGI_FORMAT_R16G16_UINT;
        case nrd::Format::RG16_SINT:
            return DXGI_FORMAT_R16G16_SINT;
        case nrd::Format::RG16_SFLOAT:
            return DXGI_FORMAT_R16G16_FLOAT;
        case nrd::Format::RGBA16_UNORM:
            return DXGI_FORMAT_R16G16B16A16_UNORM;
        case nrd::Format::RGBA16_SNORM:
            return DXGI_FORMAT_R16G16B16A16_SNORM;
        case nrd::Format::RGBA16_UINT:
            return DXGI_FORMAT_R16G16B16A16_UINT;
        case nrd::Format::RGBA16_SINT:
            return DXGI_FORMAT_R16G16B16A16_SINT;
        case nrd::Format::RGBA16_SFLOAT:
            return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case nrd::Format::R32_UINT:
            return DXGI_FORMAT_R32_UINT;
        case nrd::Format::R32_SINT:
            return DXGI_FORMAT_R32_SINT;
        case nrd::Format::R32_SFLOAT:
            return DXGI_FORMAT_R32_FLOAT;
        case nrd::Format::RG32_UINT:
            return DXGI_FORMAT_R32G32_UINT;
        case nrd::Format::RG32_SINT:
            return DXGI_FORMAT_R32G32_SINT;
        case nrd::Format::RG32_SFLOAT:
            return DXGI_FORMAT_R32G32_FLOAT;
        case nrd::Format::RGB32_UINT:
            return DXGI_FORMAT_R32G32B32_UINT;
        case nrd::Format::RGB32_SINT:
            return DXGI_FORMAT_R32G32B32_SINT;
        case nrd::Format::RGB32_SFLOAT:
            return DXGI_FORMAT_R32G32B32_FLOAT;
        case nrd::Format::RGBA32_UINT:
            return DXGI_FORMAT_R32G32B32A32_UINT;
        case nrd::Format::RGBA32_SINT:
            return DXGI_FORMAT_R32G32B32A32_SINT;
        case nrd::Format::RGBA32_SFLOAT:
            return DXGI_FORMAT_R32G32B32A32_FLOAT;
        case nrd::Format::R10_G10_B10_A2_UNORM:
            return DXGI_FORMAT_R10G10B10A2_UNORM;
        case nrd::Format::R10_G10_B10_A2_UINT:
            return DXGI_FORMAT_R10G10B10A2_UINT;
        case nrd::Format::R11_G11_B10_UFLOAT:
            return DXGI_FORMAT_R11G11B10_FLOAT;
        case nrd::Format::R9_G9_B9_E5_UFLOAT:
            return DXGI_FORMAT_R9G9B9E5_SHAREDEXP;
        default:
            return DXGI_FORMAT_UNKNOWN;
        }
    }

    void CopyMatrix(float (&dst)[16], const DirectX::XMFLOAT4X4& src)
    {
        std::memcpy(dst, &src, sizeof(float) * 16);
    }

    void SetFailure(NrdStatus& status, const std::string& fallbackReason, const std::string& lastError = {})
    {
        status.evaluationReady = false;
        status.fallbackReason = fallbackReason;
        status.lastError = lastError;
    }

    D3D12_STATIC_SAMPLER_DESC ToStaticSampler(nrd::Sampler sampler, uint32_t shaderRegister, uint32_t registerSpace)
    {
        D3D12_STATIC_SAMPLER_DESC desc = {};
        desc.Filter = sampler == nrd::Sampler::LINEAR_CLAMP ? D3D12_FILTER_MIN_MAG_MIP_LINEAR : D3D12_FILTER_MIN_MAG_MIP_POINT;
        desc.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        desc.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        desc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        desc.MipLODBias = 0.0f;
        desc.MaxAnisotropy = 1;
        desc.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        desc.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK;
        desc.MinLOD = 0.0f;
        desc.MaxLOD = D3D12_FLOAT32_MAX;
        desc.ShaderRegister = shaderRegister;
        desc.RegisterSpace = registerSpace;
        desc.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        return desc;
    }
#endif
}

NrdBackend::NrdBackend()
{
    m_status.compiled = D3D12LOOKDEVPT_WITH_NRD != 0;
    if (!m_status.compiled)
    {
        m_status.fallbackReason = "NRD was disabled at build time.";
    }
}

NrdBackend::~NrdBackend()
{
    Shutdown();
}

void NrdBackend::Initialize(ID3D12Device* device, uint32_t resourceWidth, uint32_t resourceHeight, NrdMethod method, uint32_t frameContextCount)
{
    Shutdown();
    m_status = {};
    m_status.compiled = D3D12LOOKDEVPT_WITH_NRD != 0;
    m_resourceWidth = resourceWidth;
    m_resourceHeight = resourceHeight;
    m_frameContextCount = (std::max)(frameContextCount, 1u);
    m_method = method;

#if !D3D12LOOKDEVPT_WITH_NRD
    (void)device;
    m_status.fallbackReason = "NRD was disabled at build time.";
    return;
#else
    if (!device)
    {
        m_status.fallbackReason = "D3D12 device is unavailable.";
        return;
    }

    m_status.headersAvailable = true;
    m_status.resourceWidth = resourceWidth;
    m_status.resourceHeight = resourceHeight;
    m_status.selectedDenoiser = NrdDenoiserName(method);

    const nrd::LibraryDesc* libraryDesc = nrd::GetLibraryDesc();
    if (!libraryDesc)
    {
        m_status.fallbackReason = "NRD library descriptor is unavailable.";
        m_status.lastError = "nrd::GetLibraryDesc returned null.";
        return;
    }

    m_status.libraryAvailable = true;
    m_status.versionMajor = libraryDesc->versionMajor;
    m_status.versionMinor = libraryDesc->versionMinor;
    m_status.versionBuild = libraryDesc->versionBuild;
    m_status.supportedDenoiserCount = libraryDesc->supportedDenoisersNum;
    m_status.normalEncoding = NormalEncodingName(libraryDesc->normalEncoding);
    m_status.roughnessEncoding = RoughnessEncodingName(libraryDesc->roughnessEncoding);

    const nrd::Denoiser denoiser = ToNrdDenoiser(method);
    if (!IsSupported(*libraryDesc, denoiser))
    {
        m_status.fallbackReason = "The selected NRD denoiser is not supported by this SDK build.";
        return;
    }

    nrd::DenoiserDesc denoiserDesc{};
    denoiserDesc.identifier = DenoiserIdentifier;
    denoiserDesc.denoiser = denoiser;

    nrd::InstanceCreationDesc creationDesc{};
    creationDesc.denoisers = &denoiserDesc;
    creationDesc.denoisersNum = 1;

    nrd::Instance* instance = nullptr;
    const nrd::Result createResult = nrd::CreateInstance(creationDesc, instance);
    if (createResult != nrd::Result::SUCCESS || !instance)
    {
        m_status.fallbackReason = "NRD instance creation failed.";
        m_status.lastError = std::string("nrd::CreateInstance returned ") + ResultName(createResult) + ".";
        return;
    }

    m_instance = instance;
    m_status.instanceCreated = true;

    const nrd::InstanceDesc* instanceDesc = nrd::GetInstanceDesc(*instance);
    if (instanceDesc)
    {
        m_status.pipelineCount = instanceDesc->pipelinesNum;
        m_status.permanentPoolSize = instanceDesc->permanentPoolSize;
        m_status.transientPoolSize = instanceDesc->transientPoolSize;
        m_status.constantBufferMaxDataSize = instanceDesc->constantBufferMaxDataSize;
    }

    if (!InitializeBridge(device))
    {
        return;
    }

    m_status.initialized = true;
    m_status.evaluationReady = true;
    m_status.fallbackReason.clear();
    m_status.lastError.clear();
#endif
}

void NrdBackend::Resize(ID3D12Device* device, uint32_t resourceWidth, uint32_t resourceHeight)
{
    if (resourceWidth == m_resourceWidth && resourceHeight == m_resourceHeight && m_status.initialized)
    {
        return;
    }
    Initialize(device, resourceWidth, resourceHeight, m_method, m_frameContextCount);
}

void NrdBackend::UpdateMethod(ID3D12Device* device, NrdMethod method)
{
    if (method == m_method && m_status.initialized)
    {
        return;
    }
    Initialize(device, m_resourceWidth, m_resourceHeight, method, m_frameContextCount);
}

void NrdBackend::ResetHistory()
{
    m_status.historyResetRequested = true;
}

bool NrdBackend::Evaluate(const NrdEvaluationDesc& desc)
{
#if !D3D12LOOKDEVPT_WITH_NRD
    (void)desc;
    return false;
#else
    if (!m_status.evaluationReady || !m_instance || !m_device || !m_descriptorHeap || !m_rootSignature || !m_constantBuffer || !m_mappedConstantBuffer)
    {
        return false;
    }

    if (!desc.commandList ||
        !desc.motion.resource ||
        !desc.normalRoughness.resource ||
        !desc.viewZ.resource ||
        !desc.diffuseRadianceHitDistance.resource ||
        !desc.specularRadianceHitDistance.resource ||
        !desc.diffuseOutput.resource ||
        !desc.specularOutput.resource)
    {
        SetFailure(m_status, "NRD evaluation resources are incomplete.");
        return false;
    }

    struct ExternalResource
    {
        nrd::ResourceType type;
        ID3D12Resource* resource;
        D3D12_RESOURCE_STATES state;
    };

    const bool historyConfidenceAvailable =
        desc.diffuseHistoryConfidence.resource != nullptr &&
        desc.specularHistoryConfidence.resource != nullptr;

    std::array<ExternalResource, 9> externalResources{};
    uint32_t externalResourceCount = 0;
    auto addExternal = [&](nrd::ResourceType type, ID3D12Resource* resource)
    {
        externalResources[externalResourceCount++] = ExternalResource{
            type,
            resource,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS };
    };
    addExternal(nrd::ResourceType::IN_MV, desc.motion.resource);
    addExternal(nrd::ResourceType::IN_NORMAL_ROUGHNESS, desc.normalRoughness.resource);
    addExternal(nrd::ResourceType::IN_VIEWZ, desc.viewZ.resource);
    addExternal(nrd::ResourceType::IN_DIFF_RADIANCE_HITDIST, desc.diffuseRadianceHitDistance.resource);
    addExternal(nrd::ResourceType::IN_SPEC_RADIANCE_HITDIST, desc.specularRadianceHitDistance.resource);
    addExternal(nrd::ResourceType::OUT_DIFF_RADIANCE_HITDIST, desc.diffuseOutput.resource);
    addExternal(nrd::ResourceType::OUT_SPEC_RADIANCE_HITDIST, desc.specularOutput.resource);
    if (historyConfidenceAvailable)
    {
        addExternal(nrd::ResourceType::IN_DIFF_CONFIDENCE, desc.diffuseHistoryConfidence.resource);
        addExternal(nrd::ResourceType::IN_SPEC_CONFIDENCE, desc.specularHistoryConfidence.resource);
    }

    auto findExternal = [&](nrd::ResourceType type) -> ExternalResource*
    {
        for (uint32_t i = 0; i < externalResourceCount; ++i)
        {
            ExternalResource& resource = externalResources[i];
            if (resource.type == type)
            {
                return &resource;
            }
        }
        return nullptr;
    };

    auto resourceName = [](nrd::ResourceType type) -> const char*
    {
        const char* name = nrd::GetResourceTypeString(type);
        return name ? name : "UNKNOWN";
    };

    nrd::Instance& instance = *reinterpret_cast<nrd::Instance*>(m_instance);

    nrd::CommonSettings commonSettings{};
    CopyMatrix(commonSettings.viewToClipMatrix, desc.viewToClip);
    CopyMatrix(commonSettings.viewToClipMatrixPrev, desc.viewToClipPrev);
    CopyMatrix(commonSettings.worldToViewMatrix, desc.worldToView);
    CopyMatrix(commonSettings.worldToViewMatrixPrev, desc.worldToViewPrev);
    commonSettings.motionVectorScale[0] = 1.0f;
    commonSettings.motionVectorScale[1] = 1.0f;
    commonSettings.motionVectorScale[2] = 1.0f;
    commonSettings.cameraJitter[0] = desc.cameraJitter.x;
    commonSettings.cameraJitter[1] = desc.cameraJitter.y;
    commonSettings.cameraJitterPrev[0] = desc.previousCameraJitter.x;
    commonSettings.cameraJitterPrev[1] = desc.previousCameraJitter.y;
    commonSettings.resourceSize[0] = static_cast<uint16_t>((std::min)(desc.resourceWidth, 65535u));
    commonSettings.resourceSize[1] = static_cast<uint16_t>((std::min)(desc.resourceHeight, 65535u));
    commonSettings.resourceSizePrev[0] = commonSettings.resourceSize[0];
    commonSettings.resourceSizePrev[1] = commonSettings.resourceSize[1];
    commonSettings.rectSize[0] = commonSettings.resourceSize[0];
    commonSettings.rectSize[1] = commonSettings.resourceSize[1];
    commonSettings.rectSizePrev[0] = commonSettings.resourceSize[0];
    commonSettings.rectSizePrev[1] = commonSettings.resourceSize[1];
    commonSettings.viewZScale = 1.0f;
    commonSettings.denoisingRange = desc.denoisingRange;
    commonSettings.frameIndex = desc.frameIndex;
    commonSettings.accumulationMode = m_status.historyResetRequested ? nrd::AccumulationMode::CLEAR_AND_RESTART : nrd::AccumulationMode::CONTINUE;
    commonSettings.isMotionVectorInWorldSpace = false;
    commonSettings.isHistoryConfidenceAvailable = historyConfidenceAvailable;

    nrd::Result result = nrd::SetCommonSettings(instance, commonSettings);
    if (result != nrd::Result::SUCCESS)
    {
        SetFailure(m_status, "NRD common settings update failed.", std::string("nrd::SetCommonSettings returned ") + ResultName(result) + ".");
        return false;
    }

    const nrd::Identifier identifier = DenoiserIdentifier;
    const nrd::DispatchDesc* dispatches = nullptr;
    uint32_t dispatchesNum = 0;
    result = nrd::GetComputeDispatches(instance, &identifier, 1, dispatches, dispatchesNum);
    if (result != nrd::Result::SUCCESS || !dispatches)
    {
        SetFailure(m_status, "NRD dispatch extraction failed.", std::string("nrd::GetComputeDispatches returned ") + ResultName(result) + ".");
        return false;
    }

    m_status.historyResetRequested = false;
    const uint32_t frameContextIndex = desc.frameContextIndex % m_frameContextCount;
    const uint32_t descriptorFrameBase = frameContextIndex * m_descriptorSetCount * m_descriptorSetStride;
    const uint64_t constantBufferFrameBegin = static_cast<uint64_t>(frameContextIndex) * m_constantBufferFrameStride;
    const uint64_t constantBufferFrameEnd = constantBufferFrameBegin + m_constantBufferFrameStride;
    m_constantBufferCursor = constantBufferFrameBegin;
    m_lastConstantBufferAddress = 0;

    if (m_descriptorCaches.size() != m_frameContextCount)
    {
        m_descriptorCaches.clear();
        m_descriptorCaches.resize(m_frameContextCount);
    }
    std::vector<DispatchDescriptorCache>& frameDescriptorCache = m_descriptorCaches[frameContextIndex];
    if (frameDescriptorCache.size() < dispatchesNum)
    {
        frameDescriptorCache.resize(dispatchesNum);
    }

    ID3D12DescriptorHeap* heaps[] = { m_descriptorHeap.Get() };
    desc.commandList->SetDescriptorHeaps(_countof(heaps), heaps);
    desc.commandList->SetComputeRootSignature(m_rootSignature.Get());

    auto transitionExternal = [&](ExternalResource& resource, D3D12_RESOURCE_STATES desiredState, std::vector<D3D12_RESOURCE_BARRIER>& barriers)
    {
        if (resource.resource && resource.state != desiredState)
        {
            barriers.push_back(CD3DX12_RESOURCE_BARRIER::Transition(resource.resource, resource.state, desiredState));
            resource.state = desiredState;
        }
    };

    auto transitionPool = [&](PoolTexture& resource, D3D12_RESOURCE_STATES desiredState, std::vector<D3D12_RESOURCE_BARRIER>& barriers)
    {
        if (resource.resource && resource.state != desiredState)
        {
            barriers.push_back(CD3DX12_RESOURCE_BARRIER::Transition(resource.resource.Get(), resource.state, desiredState));
            resource.state = desiredState;
        }
    };

    auto resolveResource = [&](const nrd::ResourceDesc& resourceDesc, D3D12_RESOURCE_STATES desiredState, std::vector<D3D12_RESOURCE_BARRIER>& barriers) -> ID3D12Resource*
    {
        if (resourceDesc.type == nrd::ResourceType::PERMANENT_POOL)
        {
            if (resourceDesc.indexInPool >= m_permanentPool.size())
            {
                return nullptr;
            }
            PoolTexture& texture = m_permanentPool[resourceDesc.indexInPool];
            transitionPool(texture, desiredState, barriers);
            return texture.resource.Get();
        }

        if (resourceDesc.type == nrd::ResourceType::TRANSIENT_POOL)
        {
            if (resourceDesc.indexInPool >= m_transientPool.size())
            {
                return nullptr;
            }
            PoolTexture& texture = m_transientPool[resourceDesc.indexInPool];
            transitionPool(texture, desiredState, barriers);
            return texture.resource.Get();
        }

        ExternalResource* external = findExternal(resourceDesc.type);
        if (!external)
        {
            return nullptr;
        }
        transitionExternal(*external, desiredState, barriers);
        return external->resource;
    };

    auto findResourceWithoutTransition = [&](const nrd::ResourceDesc& resourceDesc) -> ID3D12Resource*
    {
        if (resourceDesc.type == nrd::ResourceType::PERMANENT_POOL)
        {
            return resourceDesc.indexInPool < m_permanentPool.size()
                ? m_permanentPool[resourceDesc.indexInPool].resource.Get()
                : nullptr;
        }
        if (resourceDesc.type == nrd::ResourceType::TRANSIENT_POOL)
        {
            return resourceDesc.indexInPool < m_transientPool.size()
                ? m_transientPool[resourceDesc.indexInPool].resource.Get()
                : nullptr;
        }
        ExternalResource* external = findExternal(resourceDesc.type);
        return external ? external->resource : nullptr;
    };

    auto writeSrv = [&](ID3D12Resource* resource, uint32_t setBase, uint32_t index)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format = resource->GetDesc().Format;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Texture2D.MipLevels = 1;
        CD3DX12_CPU_DESCRIPTOR_HANDLE handle(m_descriptorHeap->GetCPUDescriptorHandleForHeapStart(), setBase + index, m_descriptorSize);
        m_device->CreateShaderResourceView(resource, &srvDesc, handle);
    };

    auto writeUav = [&](ID3D12Resource* resource, uint32_t setBase, uint32_t index)
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
        uavDesc.Format = resource->GetDesc().Format;
        uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        CD3DX12_CPU_DESCRIPTOR_HANDLE handle(m_descriptorHeap->GetCPUDescriptorHandleForHeapStart(), setBase + m_srvDescriptorCount + index, m_descriptorSize);
        m_device->CreateUnorderedAccessView(resource, nullptr, &uavDesc, handle);
    };

    for (uint32_t dispatchIndex = 0; dispatchIndex < dispatchesNum; ++dispatchIndex)
    {
        const nrd::DispatchDesc& dispatch = dispatches[dispatchIndex];
        if (dispatch.pipelineIndex >= m_pipelines.size() || !m_pipelines[dispatch.pipelineIndex])
        {
            SetFailure(m_status, "NRD dispatch referenced an unavailable compute pipeline.");
            return false;
        }
        if (dispatchIndex >= m_descriptorSetCount)
        {
            SetFailure(m_status, "NRD descriptor heap does not have enough per-dispatch slices.");
            return false;
        }

        const uint32_t descriptorSetBase = descriptorFrameBase + dispatchIndex * m_descriptorSetStride;
        DispatchDescriptorCache& descriptorCache = frameDescriptorCache[dispatchIndex];
        const bool descriptorLayoutMatches =
            descriptorCache.valid &&
            descriptorCache.pipelineIndex == dispatch.pipelineIndex &&
            descriptorCache.bindings.size() == dispatch.resourcesNum;
        if (!descriptorLayoutMatches)
        {
            descriptorCache.valid = false;
            descriptorCache.pipelineIndex = dispatch.pipelineIndex;
            descriptorCache.bindings.resize(dispatch.resourcesNum);
        }

        uint32_t srvIndex = 0;
        uint32_t uavIndex = 0;
        m_transitionBarriers.clear();
        m_dispatchUavResources.clear();
        for (uint32_t resourceIndex = 0; resourceIndex < dispatch.resourcesNum; ++resourceIndex)
        {
            const nrd::ResourceDesc& resourceDesc = dispatch.resources[resourceIndex];
            const bool isSrv = resourceDesc.descriptorType == nrd::DescriptorType::TEXTURE;
            const D3D12_RESOURCE_STATES desiredState = isSrv
                ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
                : D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            ID3D12Resource* resource = resolveResource(resourceDesc, desiredState, m_transitionBarriers);
            if (!resource)
            {
                std::ostringstream message;
                message << "NRD requested unsupported resource " << resourceName(resourceDesc.type) << ".";
                SetFailure(m_status, message.str());
                return false;
            }

            if (isSrv)
            {
                if (srvIndex >= m_srvDescriptorCount)
                {
                    SetFailure(m_status, "NRD SRV descriptor heap slice is too small.");
                    return false;
                }
                const DescriptorBindingCache binding{
                    resource,
                    resource->GetDesc().Format,
                    srvIndex,
                    true };
                const DescriptorBindingCache& cachedBinding = descriptorCache.bindings[resourceIndex];
                if (!descriptorLayoutMatches ||
                    cachedBinding.resource != binding.resource ||
                    cachedBinding.format != binding.format ||
                    cachedBinding.descriptorIndex != binding.descriptorIndex ||
                    cachedBinding.isSrv != binding.isSrv)
                {
                    writeSrv(resource, descriptorSetBase, srvIndex);
                    descriptorCache.bindings[resourceIndex] = binding;
                }
                ++srvIndex;
            }
            else
            {
                if (uavIndex >= m_uavDescriptorCount)
                {
                    SetFailure(m_status, "NRD UAV descriptor heap slice is too small.");
                    return false;
                }
                const DescriptorBindingCache binding{
                    resource,
                    resource->GetDesc().Format,
                    uavIndex,
                    false };
                const DescriptorBindingCache& cachedBinding = descriptorCache.bindings[resourceIndex];
                if (!descriptorLayoutMatches ||
                    cachedBinding.resource != binding.resource ||
                    cachedBinding.format != binding.format ||
                    cachedBinding.descriptorIndex != binding.descriptorIndex ||
                    cachedBinding.isSrv != binding.isSrv)
                {
                    writeUav(resource, descriptorSetBase, uavIndex);
                    descriptorCache.bindings[resourceIndex] = binding;
                }
                ++uavIndex;
                if (std::find(m_dispatchUavResources.begin(), m_dispatchUavResources.end(), resource) == m_dispatchUavResources.end())
                {
                    m_dispatchUavResources.push_back(resource);
                }
            }
        }
        descriptorCache.valid = true;

        if (!m_transitionBarriers.empty())
        {
            desc.commandList->ResourceBarrier(static_cast<UINT>(m_transitionBarriers.size()), m_transitionBarriers.data());
        }

        if (dispatch.constantBufferData && dispatch.constantBufferDataSize > 0 && !dispatch.constantBufferDataMatchesPreviousDispatch)
        {
            if (m_constantBufferCursor + m_constantBufferStride > constantBufferFrameEnd)
            {
                SetFailure(m_status, "NRD constant buffer upload ring is too small.");
                return false;
            }
            uint8_t* dst = m_mappedConstantBuffer + m_constantBufferCursor;
            std::memset(dst, 0, static_cast<size_t>(m_constantBufferStride));
            std::memcpy(dst, dispatch.constantBufferData, (std::min<uint64_t>)(dispatch.constantBufferDataSize, m_constantBufferStride));
            m_lastConstantBufferAddress = m_constantBuffer->GetGPUVirtualAddress() + m_constantBufferCursor;
            m_constantBufferCursor += m_constantBufferStride;
        }
        else if (m_lastConstantBufferAddress == 0)
        {
            std::memset(m_mappedConstantBuffer + m_constantBufferCursor, 0, static_cast<size_t>(m_constantBufferStride));
            m_lastConstantBufferAddress = m_constantBuffer->GetGPUVirtualAddress() + m_constantBufferCursor;
            m_constantBufferCursor += m_constantBufferStride;
        }

        desc.commandList->SetPipelineState(m_pipelines[dispatch.pipelineIndex].Get());
        desc.commandList->SetComputeRootConstantBufferView(0, m_lastConstantBufferAddress);
        CD3DX12_GPU_DESCRIPTOR_HANDLE srvGpuHandle(m_descriptorHeap->GetGPUDescriptorHandleForHeapStart(), descriptorSetBase, m_descriptorSize);
        CD3DX12_GPU_DESCRIPTOR_HANDLE uavGpuHandle(m_descriptorHeap->GetGPUDescriptorHandleForHeapStart(), descriptorSetBase + m_srvDescriptorCount, m_descriptorSize);
        desc.commandList->SetComputeRootDescriptorTable(1, srvGpuHandle);
        desc.commandList->SetComputeRootDescriptorTable(2, uavGpuHandle);
        desc.commandList->Dispatch(dispatch.gridWidth, dispatch.gridHeight, 1);

        if (!m_dispatchUavResources.empty())
        {
            // A UAV-to-SRV transition in the immediately following dispatch already
            // provides the required ordering and visibility. Keep explicit UAV
            // barriers for every other case, including UAV-to-UAV dependencies.
            auto nextDispatchTransitionsToSrv = [&](ID3D12Resource* resource) -> bool
            {
                if (dispatchIndex + 1 >= dispatchesNum)
                {
                    return false;
                }

                bool usedAsSrv = false;
                const nrd::DispatchDesc& nextDispatch = dispatches[dispatchIndex + 1];
                for (uint32_t i = 0; i < nextDispatch.resourcesNum; ++i)
                {
                    const nrd::ResourceDesc& nextResourceDesc = nextDispatch.resources[i];
                    if (findResourceWithoutTransition(nextResourceDesc) != resource)
                    {
                        continue;
                    }
                    if (nextResourceDesc.descriptorType != nrd::DescriptorType::TEXTURE)
                    {
                        return false;
                    }
                    usedAsSrv = true;
                }
                return usedAsSrv;
            };

            m_uavBarriers.clear();
            for (ID3D12Resource* resource : m_dispatchUavResources)
            {
                if (!nextDispatchTransitionsToSrv(resource))
                {
                    m_uavBarriers.push_back(CD3DX12_RESOURCE_BARRIER::UAV(resource));
                }
            }
            if (!m_uavBarriers.empty())
            {
                desc.commandList->ResourceBarrier(static_cast<UINT>(m_uavBarriers.size()), m_uavBarriers.data());
            }
        }
    }

    m_backToUavBarriers.clear();
    for (uint32_t i = 0; i < externalResourceCount; ++i)
    {
        transitionExternal(externalResources[i], D3D12_RESOURCE_STATE_UNORDERED_ACCESS, m_backToUavBarriers);
    }
    if (!m_backToUavBarriers.empty())
    {
        desc.commandList->ResourceBarrier(static_cast<UINT>(m_backToUavBarriers.size()), m_backToUavBarriers.data());
    }

    return true;
#endif
}

void NrdBackend::Shutdown()
{
    ReleaseBridgeResources();
#if D3D12LOOKDEVPT_WITH_NRD
    if (m_instance)
    {
        nrd::DestroyInstance(*reinterpret_cast<nrd::Instance*>(m_instance));
        m_instance = nullptr;
    }
#endif
}

bool NrdBackend::InitializeBridge(ID3D12Device* device)
{
#if !D3D12LOOKDEVPT_WITH_NRD
    (void)device;
    return false;
#else
    ReleaseBridgeResources();
    m_device = device;

    if (!m_instance || !m_device)
    {
        SetFailure(m_status, "NRD bridge cannot initialize without an instance and D3D12 device.");
        return false;
    }

    nrd::Instance& instance = *reinterpret_cast<nrd::Instance*>(m_instance);
    const nrd::InstanceDesc* instanceDesc = nrd::GetInstanceDesc(instance);
    if (!instanceDesc)
    {
        SetFailure(m_status, "NRD instance descriptor is unavailable.");
        return false;
    }

    auto failHr = [&](const char* stage, HRESULT hr)
    {
        std::ostringstream message;
        message << stage << " failed with HRESULT 0x" << std::hex << static_cast<unsigned long>(hr) << ".";
        SetFailure(m_status, "NRD D3D12 dispatch bridge initialization failed.", message.str());
    };

    m_srvDescriptorCount = (std::max)(1u, instanceDesc->descriptorPoolDesc.perSetTexturesMaxNum);
    m_uavDescriptorCount = (std::max)(1u, instanceDesc->descriptorPoolDesc.perSetStorageTexturesMaxNum);
    m_descriptorSetStride = m_srvDescriptorCount + m_uavDescriptorCount;
    m_descriptorSetCount = (std::max)(128u, (std::max)(
        instanceDesc->descriptorPoolDesc.setsMaxNum + 4u,
        instanceDesc->pipelinesNum + 4u));
    m_descriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    CD3DX12_DESCRIPTOR_RANGE srvRange;
    srvRange.Init(
        D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
        m_srvDescriptorCount,
        instanceDesc->resourcesBaseRegisterIndex,
        instanceDesc->resourcesSpaceIndex);
    CD3DX12_DESCRIPTOR_RANGE uavRange;
    uavRange.Init(
        D3D12_DESCRIPTOR_RANGE_TYPE_UAV,
        m_uavDescriptorCount,
        instanceDesc->resourcesBaseRegisterIndex,
        instanceDesc->resourcesSpaceIndex);

    CD3DX12_ROOT_PARAMETER rootParameters[3];
    rootParameters[0].InitAsConstantBufferView(
        instanceDesc->constantBufferRegisterIndex,
        instanceDesc->constantBufferAndSamplersSpaceIndex,
        D3D12_SHADER_VISIBILITY_ALL);
    rootParameters[1].InitAsDescriptorTable(1, &srvRange, D3D12_SHADER_VISIBILITY_ALL);
    rootParameters[2].InitAsDescriptorTable(1, &uavRange, D3D12_SHADER_VISIBILITY_ALL);

    std::vector<D3D12_STATIC_SAMPLER_DESC> staticSamplers;
    staticSamplers.reserve(instanceDesc->samplersNum);
    for (uint32_t i = 0; i < instanceDesc->samplersNum; ++i)
    {
        staticSamplers.push_back(ToStaticSampler(
            instanceDesc->samplers[i],
            instanceDesc->samplersBaseRegisterIndex + i,
            instanceDesc->constantBufferAndSamplersSpaceIndex));
    }

    CD3DX12_ROOT_SIGNATURE_DESC rootSignatureDesc;
    rootSignatureDesc.Init(
        _countof(rootParameters),
        rootParameters,
        static_cast<UINT>(staticSamplers.size()),
        staticSamplers.empty() ? nullptr : staticSamplers.data(),
        D3D12_ROOT_SIGNATURE_FLAG_NONE);

    ComPtr<ID3DBlob> signature;
    ComPtr<ID3DBlob> error;
    HRESULT hr = D3D12SerializeRootSignature(&rootSignatureDesc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &error);
    if (FAILED(hr))
    {
        std::string detail = error ? static_cast<const char*>(error->GetBufferPointer()) : "";
        SetFailure(m_status, "NRD root signature serialization failed.", detail);
        return false;
    }

    hr = m_device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(), IID_PPV_ARGS(&m_rootSignature));
    if (FAILED(hr))
    {
        failHr("CreateRootSignature", hr);
        return false;
    }
    m_rootSignature->SetName(L"NRD Root Signature");

    D3D12_DESCRIPTOR_HEAP_DESC descriptorHeapDesc = {};
    descriptorHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    descriptorHeapDesc.NumDescriptors = m_descriptorSetStride * m_descriptorSetCount * m_frameContextCount;
    descriptorHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    hr = m_device->CreateDescriptorHeap(&descriptorHeapDesc, IID_PPV_ARGS(&m_descriptorHeap));
    if (FAILED(hr))
    {
        failHr("CreateDescriptorHeap", hr);
        return false;
    }
    m_descriptorHeap->SetName(L"NRD Descriptor Heap");
    m_descriptorCaches.clear();
    m_descriptorCaches.resize(m_frameContextCount);
    m_transitionBarriers.reserve(m_srvDescriptorCount + m_uavDescriptorCount);
    m_uavBarriers.reserve(m_uavDescriptorCount);
    m_backToUavBarriers.reserve(9);
    m_dispatchUavResources.reserve(m_uavDescriptorCount);

    auto createPool = [&](const nrd::TextureDesc* poolDescs, uint32_t count, std::vector<PoolTexture>& pool, const wchar_t* namePrefix) -> bool
    {
        pool.resize(count);
        auto heapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
        for (uint32_t i = 0; i < count; ++i)
        {
            const DXGI_FORMAT format = ToDxgiFormat(poolDescs[i].format);
            if (format == DXGI_FORMAT_UNKNOWN)
            {
                SetFailure(m_status, "NRD requested an unsupported pool texture format.");
                return false;
            }

            const uint32_t downsampleFactor = (std::max)(1u, static_cast<uint32_t>(poolDescs[i].downsampleFactor));
            const uint32_t width = (std::max)(1u, (m_resourceWidth + downsampleFactor - 1u) / downsampleFactor);
            const uint32_t height = (std::max)(1u, (m_resourceHeight + downsampleFactor - 1u) / downsampleFactor);
            D3D12_RESOURCE_DESC resourceDesc = CD3DX12_RESOURCE_DESC::Tex2D(
                format,
                width,
                height,
                1,
                1,
                1,
                0,
                D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

            hr = m_device->CreateCommittedResource(
                &heapProps,
                D3D12_HEAP_FLAG_NONE,
                &resourceDesc,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                nullptr,
                IID_PPV_ARGS(&pool[i].resource));
            if (FAILED(hr))
            {
                failHr("CreateCommittedResource(NRD pool)", hr);
                return false;
            }
            pool[i].state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            pool[i].resource->SetName(IndexedName(namePrefix, i).c_str());
        }
        return true;
    };

    if (!createPool(instanceDesc->permanentPool, instanceDesc->permanentPoolSize, m_permanentPool, L"NRD Permanent Pool") ||
        !createPool(instanceDesc->transientPool, instanceDesc->transientPoolSize, m_transientPool, L"NRD Transient Pool"))
    {
        return false;
    }

    m_pipelines.resize(instanceDesc->pipelinesNum);
    for (uint32_t i = 0; i < instanceDesc->pipelinesNum; ++i)
    {
        const nrd::PipelineDesc& pipelineDesc = instanceDesc->pipelines[i];
        if (!pipelineDesc.computeShaderDXIL.bytecode || pipelineDesc.computeShaderDXIL.size == 0)
        {
            SetFailure(m_status, "NRD pipeline is missing DXIL compute shader bytecode.");
            return false;
        }

        D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc = {};
        psoDesc.pRootSignature = m_rootSignature.Get();
        psoDesc.CS = CD3DX12_SHADER_BYTECODE(pipelineDesc.computeShaderDXIL.bytecode, pipelineDesc.computeShaderDXIL.size);
        hr = m_device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&m_pipelines[i]));
        if (FAILED(hr))
        {
            failHr("CreateComputePipelineState(NRD)", hr);
            return false;
        }
    }

    const uint64_t maxConstantDataSize = (std::max)(1u, instanceDesc->constantBufferMaxDataSize);
    const uint64_t dispatchCapacity = (std::max<uint64_t>)(
        m_descriptorSetCount,
        (std::max<uint64_t>)(instanceDesc->descriptorPoolDesc.setsMaxNum + 4ull, instanceDesc->pipelinesNum + 4ull));
    m_constantBufferStride = AlignUp(maxConstantDataSize, D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
    m_constantBufferFrameStride = m_constantBufferStride * dispatchCapacity;
    m_constantBufferSize = m_constantBufferFrameStride * m_frameContextCount;
    auto uploadHeapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
    auto bufferDesc = CD3DX12_RESOURCE_DESC::Buffer(m_constantBufferSize);
    hr = m_device->CreateCommittedResource(
        &uploadHeapProps,
        D3D12_HEAP_FLAG_NONE,
        &bufferDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&m_constantBuffer));
    if (FAILED(hr))
    {
        failHr("CreateCommittedResource(NRD constants)", hr);
        return false;
    }
    m_constantBuffer->SetName(L"NRD Constant Upload Buffer");
    hr = m_constantBuffer->Map(0, nullptr, reinterpret_cast<void**>(&m_mappedConstantBuffer));
    if (FAILED(hr) || !m_mappedConstantBuffer)
    {
        failHr("Map(NRD constants)", hr);
        return false;
    }
    std::memset(m_mappedConstantBuffer, 0, static_cast<size_t>(m_constantBufferSize));

    nrd::Result result = nrd::Result::SUCCESS;
    if (m_method == NrdMethod::RelaxDiffuseSpecular)
    {
        nrd::RelaxSettings settings{};
        settings.atrousIterationNum = 5;
        result = nrd::SetDenoiserSettings(instance, DenoiserIdentifier, &settings);
    }
    else
    {
        nrd::ReblurSettings settings{};
        settings.maxAccumulatedFrameNum = 30;
        settings.maxFastAccumulatedFrameNum = 6;
        settings.enableAntiFirefly = true;
        result = nrd::SetDenoiserSettings(instance, DenoiserIdentifier, &settings);
    }

    if (result != nrd::Result::SUCCESS)
    {
        SetFailure(m_status, "NRD denoiser settings update failed.", std::string("nrd::SetDenoiserSettings returned ") + ResultName(result) + ".");
        return false;
    }

    return true;
#endif
}

void NrdBackend::ReleaseBridgeResources()
{
    if (m_constantBuffer && m_mappedConstantBuffer)
    {
        D3D12_RANGE emptyRange = { 0, 0 };
        m_constantBuffer->Unmap(0, &emptyRange);
    }
    m_mappedConstantBuffer = nullptr;
    m_constantBuffer.Reset();
    m_descriptorHeap.Reset();
    m_rootSignature.Reset();
    m_pipelines.clear();
    m_permanentPool.clear();
    m_transientPool.clear();
    m_descriptorCaches.clear();
    m_transitionBarriers.clear();
    m_uavBarriers.clear();
    m_backToUavBarriers.clear();
    m_dispatchUavResources.clear();
    m_device.Reset();
    m_descriptorSize = 0;
    m_srvDescriptorCount = 1;
    m_uavDescriptorCount = 1;
    m_descriptorSetStride = 2;
    m_descriptorSetCount = 1;
    m_constantBufferStride = 256;
    m_constantBufferFrameStride = 256;
    m_constantBufferSize = 256;
    m_constantBufferCursor = 0;
    m_lastConstantBufferAddress = 0;
}
