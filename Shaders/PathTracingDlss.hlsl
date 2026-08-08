#if defined(VULKAN)
#define VK_BINDING(slot, descriptorSet) [[vk::binding(slot, descriptorSet)]]
#else
#define VK_BINDING(binding, set)
#endif

#include "PathTracingSceneConstants.hlsli"

VK_BINDING(3, 0) ConstantBuffer<SceneConstants> g_scene : register(b0, space0);
VK_BINDING(13, 0) RWTexture2D<float4> g_denoiseAov0 : register(u5, space0);
VK_BINDING(14, 0) RWTexture2D<float4> g_denoiseAov1 : register(u6, space0);
VK_BINDING(15, 0) RWTexture2D<float4> g_denoiseAov2 : register(u7, space0);
VK_BINDING(28, 0) RWTexture2D<float4> g_signalResidual : register(u20, space0);
VK_BINDING(51, 0) RWTexture2D<float> g_dlssDepth : register(u47, space0);
VK_BINDING(52, 0) RWTexture2D<float2> g_dlssMotion : register(u48, space0);
VK_BINDING(53, 0) RWTexture2D<float4> g_dlssNormalRoughness : register(u49, space0);
VK_BINDING(54, 0) RWTexture2D<float4> g_dlssAlbedo : register(u50, space0);
VK_BINDING(55, 0) RWTexture2D<float4> g_dlssSpecularAlbedo : register(u51, space0);
VK_BINDING(56, 0) RWTexture2D<float> g_dlssExposure : register(u52, space0);

[numthreads(8, 8, 1)]
void DlssPrepareCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint2 dimensions =
        max((uint2)round(g_scene.renderOutputOptions.xy), uint2(1u, 1u));
    uint2 pixel = dispatchThreadId.xy;
    if (any(pixel >= dimensions))
    {
        return;
    }

    float4 normalDepth = g_denoiseAov0[pixel];
    float4 albedoRoughness = g_denoiseAov1[pixel];
    float4 motionHit = g_denoiseAov2[pixel];
    bool hit = motionHit.w > 0.0f;
    float3 normal = hit
        ? normalize(normalDepth.xyz * 2.0f - 1.0f)
        : float3(0.0f, 0.0f, 1.0f);
    float3 albedo = hit ? max(albedoRoughness.rgb, 0.0f.xxx) : 0.0f.xxx;
    float roughness = hit ? saturate(albedoRoughness.w) : 1.0f;
    float metallic = hit ? saturate(g_signalResidual[pixel].a) : 0.0f;

    g_dlssDepth[pixel] = hit
        ? max(abs(normalDepth.w), g_scene.rayOptions.x)
        : g_scene.rayOptions.y;
    g_dlssMotion[pixel] = motionHit.xy;
    g_dlssNormalRoughness[pixel] = float4(normal, roughness);
    g_dlssAlbedo[pixel] = float4(albedo, 1.0f);
    g_dlssSpecularAlbedo[pixel] =
        float4(lerp(0.04f.xxx, albedo, metallic), 1.0f);

    if (all(pixel == uint2(0u, 0u)))
    {
        // Color is tagged as pre-exposed HDR. Exposure is kept explicit so
        // Streamline's resource contract does not depend on an implicit null
        // texture or stale value from a previous viewport.
        g_dlssExposure[uint2(0u, 0u)] = 1.0f;
    }
}
