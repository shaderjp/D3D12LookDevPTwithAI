#pragma once

#include "EditorTypes.h"

#include <DirectXMath.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace rb
{
struct SceneVertex
{
    DirectX::XMFLOAT3 position;
    DirectX::XMFLOAT3 normal;
    DirectX::XMFLOAT2 texcoord;
    DirectX::XMFLOAT4 tangent = DirectX::XMFLOAT4(1.0f, 0.0f, 0.0f, 1.0f);
};

struct SceneDraw
{
    std::uint32_t indexCount = 0;
    std::uint32_t startIndex = 0;
    std::int32_t baseVertex = 0;
    std::uint32_t materialIndex = 0;
};

struct SceneMaterial
{
    MaterialAssignment assignment;
    std::wstring baseColorTexturePath;
    std::wstring normalTexturePath;
    std::wstring roughnessTexturePath;
    std::wstring metallicTexturePath;
    std::wstring occlusionTexturePath;
    std::wstring emissiveTexturePath;
    std::wstring alphaTexturePath;
    DirectX::XMFLOAT4 baseColorFactor = DirectX::XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f);
    DirectX::XMFLOAT4 emissiveFactor = DirectX::XMFLOAT4(0.0f, 0.0f, 0.0f, 1.0f);
    bool hasBaseColorTexture = false;
    bool hasNormalTexture = false;
    bool hasRoughnessTexture = false;
    bool hasMetallicTexture = false;
    bool hasOcclusionTexture = false;
    bool hasEmissiveTexture = false;
    bool hasAlphaTexture = false;
    bool twoSidedEmission = false;
    float transmissionFactor = 0.0f;
    float indexOfRefraction = 1.5f;
    bool thinDielectric = false;
    DirectX::XMFLOAT4 uvScaleOffset = DirectX::XMFLOAT4(1.0f, 1.0f, 0.0f, 0.0f);
};

struct SceneMesh
{
    std::string name;
    std::vector<SceneVertex> vertices;
    std::vector<std::uint32_t> indices;
    std::vector<SceneDraw> draws;
    DirectX::XMFLOAT3 boundsMin = DirectX::XMFLOAT3(0.0f, 0.0f, 0.0f);
    DirectX::XMFLOAT3 boundsMax = DirectX::XMFLOAT3(0.0f, 0.0f, 0.0f);
};

struct SceneInstance
{
    std::uint32_t meshIndex = 0;
    DirectX::XMFLOAT4X4 transform = {};
    DirectX::XMFLOAT4X4 normalTransform = {};
};

struct SceneCamera
{
    DirectX::XMFLOAT3 position = DirectX::XMFLOAT3(0.0f, 0.0f, 0.0f);
    DirectX::XMFLOAT3 forward = DirectX::XMFLOAT3(0.0f, 0.0f, 1.0f);
    DirectX::XMFLOAT3 up = DirectX::XMFLOAT3(0.0f, 1.0f, 0.0f);
    float fovDegrees = 60.0f;
};

enum class SceneLightType : std::uint32_t
{
    Point,
    Spot,
    Distant,
};

struct SceneLight
{
    SceneLightType type = SceneLightType::Point;
    DirectX::XMFLOAT3 position = DirectX::XMFLOAT3(0.0f, 0.0f, 0.0f);
    DirectX::XMFLOAT3 direction = DirectX::XMFLOAT3(0.0f, 0.0f, 1.0f);
    DirectX::XMFLOAT3 radiance = DirectX::XMFLOAT3(1.0f, 1.0f, 1.0f);
    float coneAngleDegrees = 30.0f;
    float coneDeltaDegrees = 5.0f;
};

struct SceneEnvironment
{
    std::wstring texturePath;
    DirectX::XMFLOAT3 scale = DirectX::XMFLOAT3(1.0f, 1.0f, 1.0f);
    DirectX::XMFLOAT4X4 lightToWorld = {};
    // PBRT v4 tabulates infinite lights with its equal-area square/sphere
    // mapping. Other import paths keep the renderer's legacy lat-long layout.
    bool equalAreaMapping = false;
};

struct ImportedScene
{
    std::wstring path;
    std::vector<SceneVertex> vertices;
    std::vector<std::uint32_t> indices;
    std::vector<SceneDraw> draws;
    std::vector<SceneMaterial> materials;
    // Canonical representation used by PBRT and by the DXR instance path.
    // Legacy importers retain the flat arrays above and are normalized to a
    // single mesh and identity instance by SceneImporter.
    std::vector<SceneMesh> meshes;
    std::vector<SceneInstance> instances;
    std::optional<SceneCamera> camera;
    std::optional<SceneEnvironment> environment;
    std::vector<SceneLight> lights;
    bool hasAuthoredLighting = false;
    DirectX::XMFLOAT3 boundsMin = DirectX::XMFLOAT3(0.0f, 0.0f, 0.0f);
    DirectX::XMFLOAT3 boundsMax = DirectX::XMFLOAT3(0.0f, 0.0f, 0.0f);
};
}
