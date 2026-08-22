#ifndef VK_BINDING
#define VK_BINDING(binding, set)
#endif

#include "PathTracingSceneConstants.hlsli"

struct SecondaryTask
{
    uint pixelIndex;
    uint sampleIndex;
};

struct SecondaryResult
{
    float4 radiance;
    uint4 packedSignals;
};

VK_BINDING(1, 0) RWTexture2D<float4> g_output : register(u0, space0);
VK_BINDING(2, 0) RWTexture2D<float4> g_accumulation : register(u1, space0);
VK_BINDING(13, 0) RWTexture2D<float4> g_denoiseAov0 : register(u5, space0);
VK_BINDING(14, 0) RWTexture2D<float4> g_denoiseAov1 : register(u6, space0);
VK_BINDING(15, 0) RWTexture2D<float4> g_denoiseAov2 : register(u7, space0);
VK_BINDING(17, 0) RWTexture2D<float4> g_reconstructionHistoryMoments : register(u9, space0);
VK_BINDING(18, 0) RWTexture2D<float4> g_reconstructionHistoryLength : register(u10, space0);
VK_BINDING(26, 0) RWTexture2D<float4> g_signalDiffuse : register(u18, space0);
VK_BINDING(27, 0) RWTexture2D<float4> g_signalSpecular : register(u19, space0);
VK_BINDING(28, 0) RWTexture2D<float4> g_signalResidual : register(u20, space0);
VK_BINDING(39, 0) RWTexture2D<float4> g_reconstructionHistoryMomentsB : register(u31, space0);
VK_BINDING(40, 0) RWTexture2D<float4> g_reconstructionHistoryLengthB : register(u32, space0);
VK_BINDING(41, 0) RWTexture2D<float4> g_postDenoiseHdr : register(u33, space0);
VK_BINDING(44, 0) RWTexture2D<float> g_diffuseHistoryConfidence : register(u36, space0);
VK_BINDING(45, 0) RWTexture2D<float> g_specularHistoryConfidence : register(u37, space0);
VK_BINDING(57, 0) RWTexture2D<float4> g_primaryPositionCone : register(u53, space0);
VK_BINDING(58, 0) RWTexture2D<float4> g_primaryGeometricNormal : register(u54, space0);
VK_BINDING(59, 0) RWTexture2D<uint4> g_primaryIdentity : register(u55, space0);
VK_BINDING(60, 0) RWStructuredBuffer<uint> g_secondaryTaskOffsets : register(u56, space0);
VK_BINDING(61, 0) RWStructuredBuffer<uint> g_secondaryGroupOffsets : register(u57, space0);
VK_BINDING(62, 0) RWStructuredBuffer<SecondaryTask> g_secondaryTasks : register(u58, space0);
VK_BINDING(63, 0) RWStructuredBuffer<SecondaryResult> g_secondaryResults : register(u59, space0);
VK_BINDING(64, 0) RWByteAddressBuffer g_secondaryIndirectArgs : register(u60, space0);
VK_BINDING(3, 0) ConstantBuffer<SceneConstants> g_scene : register(b0, space0);

groupshared uint g_groupTaskCounts[256];

uint2 RenderDimensions()
{
    return (uint2)round(g_scene.rayOptions.zw);
}

uint PixelIndex(uint2 pixel, uint2 dimensions)
{
    return pixel.y * dimensions.x + pixel.x;
}

uint RequiredSecondarySamples(uint2 pixel)
{
    uint baseSamples = clamp((uint)round(g_scene.giOptions.x), 1u, 4u);
    if (g_scene.adaptiveOptions.x > 0.5f)
    {
        uint historyDomains = (uint)round(g_scene.environmentOptions.w);
        bool historyValid =
            (historyDomains & (HISTORY_DOMAIN_SURFACE | HISTORY_DOMAIN_DENOISER)) ==
            (HISTORY_DOMAIN_SURFACE | HISTORY_DOMAIN_DENOISER);
        float4 historyLength = 0.0f.xxxx;
        float4 moments = 0.0f.xxxx;
        if (historyValid)
        {
            bool previousIsA = (g_scene.frameOptions.w & 1u) == 0u;
            historyLength = previousIsA
                ? g_reconstructionHistoryLength[pixel]
                : g_reconstructionHistoryLengthB[pixel];
            moments = previousIsA
                ? g_reconstructionHistoryMoments[pixel]
                : g_reconstructionHistoryMomentsB[pixel];
        }
        float diffuseVariance = max(moments.y - moments.x * moments.x, 0.0f);
        float specularVariance = max(moments.w - moments.z * moments.z, 0.0f);
        if (!historyValid ||
            min(historyLength.x, historyLength.y) < 2.0f ||
            max(diffuseVariance, specularVariance) > g_scene.adaptiveOptions.z ||
            max(historyLength.z, historyLength.w) > 0.5f)
        {
            baseSamples = max(
                baseSamples,
                clamp((uint)round(g_scene.adaptiveOptions.y), 1u, 4u));
        }
    }

    if (g_scene.performanceOptions.y < 0.75f &&
        ((pixel.x + pixel.y + g_scene.frameOptions.w) & 1u) != 0u)
    {
        float roughness = g_denoiseAov1[pixel].w;
        float metallic = g_signalResidual[pixel].w;
        float confidence = max(
            g_diffuseHistoryConfidence[pixel],
            g_specularHistoryConfidence[pixel]);
        if (roughness >= 0.15f && metallic <= 0.5f && confidence >= 0.25f)
        {
            return 0u;
        }
    }
    return baseSamples;
}

[numthreads(256, 1, 1)]
void SecondaryTaskCountCS(
    uint3 dispatchThreadId : SV_DispatchThreadID,
    uint3 groupThreadId : SV_GroupThreadID,
    uint3 groupId : SV_GroupID)
{
    uint2 dimensions = RenderDimensions();
    uint pixelCount = dimensions.x * dimensions.y;
    uint linearIndex = dispatchThreadId.x;
    uint taskCount = 0u;
    if (linearIndex < pixelCount)
    {
        uint2 pixel = uint2(
            linearIndex % dimensions.x,
            linearIndex / dimensions.x);
        if (g_denoiseAov2[pixel].w > 0.0f)
        {
            taskCount = RequiredSecondarySamples(pixel);
        }
    }

    uint lane = groupThreadId.x;
    g_groupTaskCounts[lane] = taskCount;
    GroupMemoryBarrierWithGroupSync();
    [unroll]
    for (uint offset = 1u; offset < 256u; offset <<= 1u)
    {
        uint addend = lane >= offset
            ? g_groupTaskCounts[lane - offset]
            : 0u;
        GroupMemoryBarrierWithGroupSync();
        g_groupTaskCounts[lane] += addend;
        GroupMemoryBarrierWithGroupSync();
    }

    if (linearIndex < pixelCount)
    {
        uint exclusivePrefix = lane == 0u
            ? 0u
            : g_groupTaskCounts[lane - 1u];
        g_secondaryTaskOffsets[linearIndex] =
            (min(taskCount, 255u) << 24u) |
            min(exclusivePrefix, 0x00ffffffu);
    }
    if (lane == 255u)
    {
        g_secondaryGroupOffsets[groupId.x] = g_groupTaskCounts[255u];
    }
}

[numthreads(1, 1, 1)]
void SecondaryGroupScanCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint groupCount;
    uint groupStride;
    g_secondaryGroupOffsets.GetDimensions(groupCount, groupStride);
    uint runningTotal = 0u;
    for (uint groupIndex = 0u; groupIndex < groupCount; ++groupIndex)
    {
        uint groupTaskCount = g_secondaryGroupOffsets[groupIndex];
        g_secondaryGroupOffsets[groupIndex] = runningTotal;
        runningTotal += groupTaskCount;
    }

    uint taskCapacity;
    uint taskStride;
    g_secondaryTasks.GetDimensions(taskCapacity, taskStride);
    runningTotal = min(runningTotal, taskCapacity);
    if (runningTotal == 0u)
    {
        SecondaryTask dummy;
        dummy.pixelIndex = 0xffffffffu;
        dummy.sampleIndex = 0u;
        g_secondaryTasks[0] = dummy;
    }
    // D3D12_DISPATCH_RAYS_DESC::Width/Height/Depth start at byte 88.
    g_secondaryIndirectArgs.Store(88u, max(runningTotal, 1u));
    g_secondaryIndirectArgs.Store(92u, 1u);
    g_secondaryIndirectArgs.Store(96u, 1u);
}

[numthreads(256, 1, 1)]
void SecondaryTaskScatterCS(
    uint3 dispatchThreadId : SV_DispatchThreadID,
    uint3 groupId : SV_GroupID)
{
    uint2 dimensions = RenderDimensions();
    uint pixelCount = dimensions.x * dimensions.y;
    uint linearIndex = dispatchThreadId.x;
    if (linearIndex >= pixelCount)
    {
        return;
    }

    uint packedPrefix = g_secondaryTaskOffsets[linearIndex];
    uint taskCount = packedPrefix >> 24u;
    uint taskStart =
        g_secondaryGroupOffsets[groupId.x] +
        (packedPrefix & 0x00ffffffu);
    uint taskCapacity;
    uint taskStride;
    g_secondaryTasks.GetDimensions(taskCapacity, taskStride);
    uint firstSampleIndex = g_scene.frameOptions.w * 32u;
    for (uint sample = 0u; sample < taskCount; ++sample)
    {
        uint taskIndex = taskStart + sample;
        if (taskIndex >= taskCapacity)
        {
            break;
        }
        SecondaryTask task;
        task.pixelIndex = linearIndex;
        task.sampleIndex = firstSampleIndex + sample;
        g_secondaryTasks[taskIndex] = task;
    }
}

float2 UnpackHalf2(uint packed)
{
    return float2(
        f16tof32(packed & 0xffffu),
        f16tof32(packed >> 16u));
}

float3 AcesTonemap(float3 value)
{
    const float a = 2.51f;
    const float b = 0.03f;
    const float c = 2.43f;
    const float d = 0.59f;
    const float e = 0.14f;
    return saturate(
        (value * (a * value + b)) /
        (value * (c * value + d) + e));
}

float3 Tonemap(float3 value)
{
    value = max(value * exp2(g_scene.viewOptions.x), 0.0f.xxx);
    uint toneMapper = (uint)round(g_scene.viewOptions.z);
    if (toneMapper == 1u)
    {
        value = value / (1.0f.xxx + value);
    }
    else if (toneMapper == 2u)
    {
        value = AcesTonemap(value);
    }
    return pow(
        saturate(value),
        1.0f / max(g_scene.viewOptions.y, 0.01f));
}

[numthreads(8, 8, 1)]
void SecondaryResolveCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint2 pixel = dispatchThreadId.xy;
    uint2 dimensions = RenderDimensions();
    if (any(pixel >= dimensions))
    {
        return;
    }

    uint linearIndex = PixelIndex(pixel, dimensions);
    uint packedPrefix = g_secondaryTaskOffsets[linearIndex];
    uint taskCount = packedPrefix >> 24u;
    uint groupIndex = linearIndex / 256u;
    uint taskStart =
        g_secondaryGroupOffsets[groupIndex] +
        (packedPrefix & 0x00ffffffu);
    uint resultCapacity;
    uint resultStride;
    g_secondaryResults.GetDimensions(resultCapacity, resultStride);
    taskCount = min(taskCount, resultCapacity > taskStart
        ? resultCapacity - taskStart
        : 0u);

    float3 color = g_signalDiffuse[pixel].rgb +
        g_signalSpecular[pixel].rgb +
        g_signalResidual[pixel].rgb;
    float3 diffuse = g_signalDiffuse[pixel].rgb;
    float3 specular = g_signalSpecular[pixel].rgb;
    float2 hitDistances = float2(
        g_signalDiffuse[pixel].a,
        g_signalSpecular[pixel].a);
    if (taskCount > 0u)
    {
        color = 0.0f.xxx;
        diffuse = 0.0f.xxx;
        specular = 0.0f.xxx;
        hitDistances = 0.0f.xx;
        for (uint taskOffset = 0u; taskOffset < taskCount; ++taskOffset)
        {
            SecondaryResult result =
                g_secondaryResults[taskStart + taskOffset];
            float2 diffuseXY = UnpackHalf2(result.packedSignals.x);
            float2 diffuseZSpecularX =
                UnpackHalf2(result.packedSignals.y);
            float2 specularYZ = UnpackHalf2(result.packedSignals.z);
            color += result.radiance.rgb;
            diffuse += float3(
                diffuseXY,
                diffuseZSpecularX.x);
            specular += float3(
                diffuseZSpecularX.y,
                specularYZ);
            hitDistances += UnpackHalf2(result.packedSignals.w);
        }
        float reciprocalCount = rcp((float)taskCount);
        color *= reciprocalCount;
        diffuse *= reciprocalCount;
        specular *= reciprocalCount;
        hitDistances *= reciprocalCount;
    }

    float3 residual = max(color - diffuse - specular, 0.0f.xxx);
    float metallic = g_signalResidual[pixel].w;
    g_signalDiffuse[pixel] = float4(max(diffuse, 0.0f.xxx), hitDistances.x);
    g_signalSpecular[pixel] = float4(max(specular, 0.0f.xxx), hitDistances.y);
    g_signalResidual[pixel] = float4(residual, metallic);
    g_diffuseHistoryConfidence[pixel] =
        hitDistances.x > 0.0f ? 1.0f : 0.0f;
    g_specularHistoryConfidence[pixel] =
        hitDistances.y > 0.0f
        ? lerp(0.55f, 1.0f, saturate(g_denoiseAov1[pixel].w))
        : 0.0f;

    // postProcessOptions.z: 1 = Baseline compact path, 2 = ReSTIR DI compact
    // path. RTXDI DI is frame-local; Baseline retains progressive accumulation.
    float3 resolvedColor = color;
    if (g_scene.postProcessOptions.z < 1.5f)
    {
        if (g_scene.frameOptions.z != 0u)
        {
            resolvedColor = g_accumulation[pixel].rgb;
        }
        else
        {
            uint accumulatedFrames = g_scene.frameOptions.x;
            uint maxAccumulatedFrames = max(g_scene.frameOptions.y, 1u);
            float weight = 1.0f /
                (float)(min(
                    accumulatedFrames,
                    maxAccumulatedFrames - 1u) + 1u);
            resolvedColor = lerp(
                g_accumulation[pixel].rgb,
                color,
                weight);
        }
    }
    g_accumulation[pixel] = float4(resolvedColor, 1.0f);
    if (g_scene.postProcessOptions.x < 0.5f)
    {
        g_postDenoiseHdr[pixel] =
            float4(max(resolvedColor, 0.0f.xxx), 1.0f);
    }
    g_output[pixel] = float4(Tonemap(resolvedColor), 1.0f);
}
