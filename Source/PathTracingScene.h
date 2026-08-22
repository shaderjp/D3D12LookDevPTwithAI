#pragma once

#include <DirectXMath.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace Bistro
{
    static constexpr uint32_t TextureSlotCount = 7;
    static constexpr uint32_t TextureSlotBaseColor = 0;
    static constexpr uint32_t TextureSlotNormal = 1;
    static constexpr uint32_t TextureSlotRoughness = 2;
    static constexpr uint32_t TextureSlotMetallic = 3;
    static constexpr uint32_t TextureSlotOcclusion = 4;
    static constexpr uint32_t TextureSlotEmissive = 5;
    static constexpr uint32_t TextureSlotAlpha = 6;

    enum RtMaterialFeature : uint32_t
    {
        RtMaterialFeaturePackedOcclusionRoughnessMetallic = 1u << 0,
        RtMaterialFeatureBaseColorTexture = 1u << 1,
        RtMaterialFeatureNormalTexture = 1u << 2,
        RtMaterialFeatureRoughnessTexture = 1u << 3,
        RtMaterialFeatureMetallicTexture = 1u << 4,
        RtMaterialFeatureOcclusionTexture = 1u << 5,
        RtMaterialFeatureEmissiveTexture = 1u << 6,
        RtMaterialFeatureAlphaTexture = 1u << 7,
    };

    struct Vertex
    {
        DirectX::XMFLOAT3 position;
        DirectX::XMFLOAT3 normal;
        DirectX::XMFLOAT4 tangent;
        DirectX::XMFLOAT2 texcoord;
    };

    struct Material
    {
        std::wstring name;
        std::array<std::wstring, TextureSlotCount> textures;
        DirectX::XMFLOAT4 baseColorFactor = DirectX::XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f);
        DirectX::XMFLOAT4 emissiveFactor = DirectX::XMFLOAT4(0.0f, 0.0f, 0.0f, 0.0f);
        float roughnessFactor = 0.48f;
        float metallicFactor = 0.0f;
        float occlusionStrength = 1.0f;
        float normalStrength = 1.0f;
        float alphaCutoff = 0.33f;
        bool alphaMasked = false;
        bool twoSidedEmission = false;
        bool packedOcclusionRoughnessMetallic = false;
        float transmissionFactor = 0.0f;
        float indexOfRefraction = 1.5f;
        bool thinDielectric = false;
        DirectX::XMFLOAT4 uvScaleOffset = DirectX::XMFLOAT4(1.0f, 1.0f, 0.0f, 0.0f);
    };

    struct DrawItem
    {
        uint32_t indexCount = 0;
        uint32_t startIndex = 0;
        int32_t baseVertex = 0;
        uint32_t materialIndex = 0;
    };

    struct MeshRange
    {
        uint32_t vertexOffset = 0;
        uint32_t vertexCount = 0;
        uint32_t indexOffset = 0;
        uint32_t indexCount = 0;
        uint32_t drawOffset = 0;
        uint32_t drawCount = 0;
        DirectX::XMFLOAT3 boundsMin = DirectX::XMFLOAT3(0.0f, 0.0f, 0.0f);
        DirectX::XMFLOAT3 boundsMax = DirectX::XMFLOAT3(0.0f, 0.0f, 0.0f);
    };

    struct SceneInstance
    {
        uint32_t meshIndex = 0;
        DirectX::XMFLOAT4X4 transform = {};
        DirectX::XMFLOAT4X4 normalTransform = {};
    };

    enum class AnalyticLightType : uint32_t
    {
        Point,
        Spot,
        Distant,
    };

    struct AnalyticLight
    {
        AnalyticLightType type = AnalyticLightType::Point;
        DirectX::XMFLOAT3 position = DirectX::XMFLOAT3(0.0f, 0.0f, 0.0f);
        DirectX::XMFLOAT3 direction = DirectX::XMFLOAT3(0.0f, 0.0f, 1.0f);
        DirectX::XMFLOAT3 radiance = DirectX::XMFLOAT3(1.0f, 1.0f, 1.0f);
        float coneAngleDegrees = 30.0f;
        float coneDeltaDegrees = 5.0f;
    };

    struct RtMaterial
    {
        DirectX::XMFLOAT4 baseColorFactor = DirectX::XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f);
        DirectX::XMFLOAT4 emissiveFactor = DirectX::XMFLOAT4(0.0f, 0.0f, 0.0f, 0.0f);
        uint32_t textureBaseIndex = 0;
        uint32_t alphaMasked = 0;
        float alphaCutoff = 0.33f;
        float normalStrength = 1.0f;
        float roughnessFactor = 0.48f;
        float metallicFactor = 0.0f;
        float occlusionStrength = 1.0f;
        uint32_t materialFeatures = 0;
        float transmissionFactor = 0.0f;
        float indexOfRefraction = 1.5f;
        uint32_t thinDielectric = 0;
        uint32_t padding = 0;
        DirectX::XMFLOAT4 uvScaleOffset = DirectX::XMFLOAT4(1.0f, 1.0f, 0.0f, 0.0f);
    };

    static_assert(sizeof(RtMaterial) == 96, "RtMaterial must match the HLSL StructuredBuffer ABI.");
    static_assert(offsetof(RtMaterial, materialFeatures) == 60, "RtMaterial material feature ABI offset changed.");
    static_assert(offsetof(RtMaterial, transmissionFactor) == 64, "RtMaterial transmission ABI offset changed.");
    static_assert(offsetof(RtMaterial, uvScaleOffset) == 80, "RtMaterial UV-transform ABI offset changed.");

    struct RtGeometryRecord
    {
        uint32_t indexOffset = 0;
        uint32_t indexCount = 0;
        int32_t baseVertex = 0;
        uint32_t materialIndex = 0;
    };

    struct RtInstance
    {
        DirectX::XMFLOAT4 objectToWorldColumn0;
        DirectX::XMFLOAT4 objectToWorldColumn1;
        DirectX::XMFLOAT4 objectToWorldColumn2;
        DirectX::XMFLOAT4 objectToWorldColumn3;
        DirectX::XMFLOAT4 normalToWorldColumn0;
        DirectX::XMFLOAT4 normalToWorldColumn1;
        DirectX::XMFLOAT4 normalToWorldColumn2;
        DirectX::XMFLOAT4 normalToWorldColumn3;
    };

    static_assert(sizeof(RtInstance) == 128, "RtInstance must match the HLSL StructuredBuffer ABI.");

    struct RtLight
    {
        DirectX::XMFLOAT4 positionArea = DirectX::XMFLOAT4(0.0f, 0.0f, 0.0f, 1.0f);
        DirectX::XMFLOAT4 edge0Type = DirectX::XMFLOAT4(1.0f, 0.0f, 0.0f, 0.0f);
        // edge1.w bit-packs a 16-bit Walker-alias acceptance threshold and a
        // 16-bit alias index without increasing the GPU light record stride.
        DirectX::XMFLOAT4 edge1 = DirectX::XMFLOAT4(0.0f, 1.0f, 0.0f, 0.0f);
        // RGB is analytic emitted radiance or a mesh importance estimate;
        // W is the exact discrete selection PDF.
        DirectX::XMFLOAT4 radianceCdf = DirectX::XMFLOAT4(0.0f, 0.0f, 0.0f, 1.0f);
        // Mesh emitters retain the exact geometry/primitive/material identity
        // needed to evaluate their emissive texture at the sampled UV. Analytic
        // lights use the all-ones sentinel and continue to use radianceCdf.rgb.
        DirectX::XMUINT4 meshIdentity = DirectX::XMUINT4(0xffffffffu, 0xffffffffu, 0xffffffffu, 0xffffffffu);
    };

    static_assert(sizeof(RtLight) == 80, "RtLight must match the HLSL StructuredBuffer ABI.");
    static_assert(offsetof(RtLight, meshIdentity) == 64, "RtLight mesh identity ABI offset changed.");

    struct LightBuildResult
    {
        std::vector<RtLight> lights;
        uint32_t activeLightCount = 0;
        uint32_t emissiveTriangleCount = 0;
        uint32_t proceduralAreaCount = 0;
    };

    struct Scene
    {
        std::wstring assetRoot;
        std::vector<Vertex> vertices;
        std::vector<uint32_t> indices;
        std::vector<Material> materials;
        std::vector<DrawItem> draws;
        std::vector<MeshRange> meshes;
        std::vector<SceneInstance> instances;
        std::vector<AnalyticLight> analyticLights;
        bool hasAuthoredLighting = false;
        DirectX::XMFLOAT3 boundsMin = DirectX::XMFLOAT3(0.0f, 0.0f, 0.0f);
        DirectX::XMFLOAT3 boundsMax = DirectX::XMFLOAT3(0.0f, 0.0f, 0.0f);
    };

    std::wstring FindAssetRoot();
    std::wstring FindEnvironmentMapPath(const std::wstring& assetRoot);
    Scene LoadScene(const std::wstring& assetRoot);
    LightBuildResult BuildLightList(const Scene& scene, uint32_t maxEmissiveTriangleLights = 2048);
    std::wstring GetRepoRootFromExecutable(uint32_t levelsFromExeToRepoRoot);
}
