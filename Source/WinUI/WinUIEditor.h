#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace lookdevpt::winui
{
using EditorValue = std::variant<
    std::monostate,
    bool,
    std::int64_t,
    double,
    std::string,
    std::wstring,
    std::array<double, 3>,
    std::array<double, 4>>;

enum class EditorCommandType : std::uint8_t
{
    SetValue,
    Action,
    LoadScene,
    LoadEnvironment,
    LoadProject,
    SaveProjectAs,
    LoadMaterialTexture,
    Pointer,
    Key,
};

enum class PointerEventType : std::uint8_t
{
    Pressed,
    Moved,
    Released,
    Exited,
    Wheel,
};

struct PointerInput
{
    PointerEventType type{};
    float x = 0.0f;
    float y = 0.0f;
    float wheelDelta = 0.0f;
    bool leftButton = false;
    bool middleButton = false;
    bool rightButton = false;
};

struct KeyInput
{
    std::uint32_t virtualKey = 0;
    bool down = false;
};

struct EditorCommand
{
    EditorCommandType type = EditorCommandType::Action;
    std::wstring property;
    EditorValue value;
    std::wstring path;
    std::int32_t index = -1;
    std::uint64_t id = 0;
    PointerInput pointer;
    KeyInput key;
};

struct MaterialItem
{
    std::wstring name;
    std::wstring detail;
};

struct TextureSlotItem
{
    std::wstring name;
    std::wstring sourcePath;
    std::wstring currentPath;
    std::wstring status;
    std::int32_t resolutionPolicy = 0;
};

struct NamedItem
{
    std::wstring name;
    std::wstring detail;
};

struct ApprovalItem
{
    std::uint64_t id = 0;
    std::wstring tool;
    std::wstring summary;
    std::int32_t secondsRemaining = 0;
};

struct EditorSnapshot
{
    std::uint64_t revision = 0;
    bool rendererReady = false;
    bool rendererStopped = false;
    bool benchmarkFinished = false;
    bool projectDirty = false;
    bool mcpRunning = false;
    bool renderOnly = false;
    std::wstring sceneName = L"Preview cube";
    std::wstring projectName;
    std::wstring status = L"Renderer starting";
    std::wstring diagnostics;
    std::wstring stats;
    std::wstring mcpEndpoint = L"http://127.0.0.1:8777/mcp";
    std::wstring mcpToken;
    std::wstring mcpLastError;
    std::vector<std::wstring> recentRequests;
    std::unordered_map<std::wstring, EditorValue> values;
    std::vector<MaterialItem> materials;
    std::vector<TextureSlotItem> textureSlots;
    std::vector<NamedItem> variants;
    std::vector<NamedItem> presets;
    std::vector<ApprovalItem> approvals;
};

using EditorSnapshotPtr = std::shared_ptr<EditorSnapshot const>;
}
