#pragma once

#include "AssistantProtocol.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace lookdevpt::assistant
{
enum class AssistantHostBridgeState : std::uint8_t
{
    Stopped,
    Starting,
    WaitingForConnection,
    Connected,
    Failed,
};

struct AssistantHostBridgeStateUpdate
{
    AssistantHostBridgeState state =
        AssistantHostBridgeState::Stopped;
    std::string diagnostic;
    // Present only after StopAndJoin has reaped the owned ChatHost process.
    // This intentionally exposes neither the child PID nor its process handle.
    std::optional<std::uint32_t> childExitCode;
};

struct AssistantHostBridgeOptions
{
    // An empty path resolves to D3D12LookDevPTwithAI.ChatHost.exe beside the
    // native executable. A relative path is also resolved from that directory.
    std::filesystem::path chatHostExecutable;
    std::chrono::milliseconds connectionTimeout{
        std::chrono::seconds(10)};
    std::size_t maximumQueuedFrames = 64;
    std::size_t maximumQueuedBytes =
        16u * 1024u * 1024u;
};

// Both callbacks run on bridge-owned worker threads. They must return quickly;
// a WinUI consumer should copy/move the value into DispatcherQueue rather than
// touching XAML from the callback.
using AssistantEventSink =
    std::function<void(AssistantEnvelope)>;
using AssistantHostStateSink =
    std::function<void(AssistantHostBridgeStateUpdate)>;

class AssistantHostBridge final
{
public:
    AssistantHostBridge();
    ~AssistantHostBridge();

    AssistantHostBridge(AssistantHostBridge const&) = delete;
    AssistantHostBridge& operator=(AssistantHostBridge const&) = delete;

    // Starts one ChatHost process. Startup failures are reported to stateSink
    // and thrown to the caller. After an asynchronous failure, call
    // StopAndJoin before starting a fresh bridge instance/session.
    void Start(
        AssistantHostBridgeOptions options,
        AssistantEventSink eventSink,
        AssistantHostStateSink stateSink);

    // Validates and frames one JSON envelope before accepting it. Returns false
    // when the bridge is stopped/failed or its bounded queue has no capacity.
    // Protocol violations throw ProtocolError without exposing the payload.
    [[nodiscard]] bool Post(std::string_view jsonEnvelope);

    // A queued shutdown request receives up to two seconds for cooperative
    // child exit. Otherwise (or on timeout), pending pipe I/O is cancelled and
    // the owned Job Object process tree is terminated before both threads join.
    void StopAndJoin() noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};
}
