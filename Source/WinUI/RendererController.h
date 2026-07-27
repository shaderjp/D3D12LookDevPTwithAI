#pragma once

#include "WinUIEditor.h"
#include "RendererCommandQueue.h"
#include "RendererSnapshotMailbox.h"

#include <wrl/client.h>
#include <dxgi1_4.h>

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

struct ISwapChainPanelNative;
class D3D12PathTracingBackend;

namespace lookdevpt::winui
{
class RendererController final
{
public:
    RendererController();
    ~RendererController();

    RendererController(RendererController const&) = delete;
    RendererController& operator=(RendererController const&) = delete;

    void Start();
    void RequestRenderStopAndWait() noexcept;
    void StopAndJoin() noexcept;
    void Enqueue(EditorCommand command);
    void SetViewportFocused(bool focused);

    [[nodiscard]] EditorSnapshotPtr LatestSnapshot() const noexcept;
    [[nodiscard]] bool AttachViewport(
        ISwapChainPanelNative* panel,
        std::wstring& diagnostics);
    void DetachViewport(
        ISwapChainPanelNative* panel,
        std::wstring& diagnostics) noexcept;

private:
    void RenderMain(std::stop_token stopToken) noexcept;
    void DrainCommands(D3D12PathTracingBackend& renderer);
    void Publish(D3D12PathTracingBackend& renderer);
    void PublishFailure(std::wstring diagnostics) noexcept;

    mutable std::mutex m_stateMutex;
    std::condition_variable m_shutdownCondition;
    RendererCommandQueue m_commands;
    RendererSnapshotMailbox m_snapshot;
    Microsoft::WRL::ComPtr<IDXGISwapChain3> m_swapChain;
    std::jthread m_renderThread;
    std::atomic_bool m_started = false;
    std::atomic_bool m_viewportFocused = false;
    bool m_renderLoopStopped = false;
    bool m_viewportDetached = true;
    bool m_threadFinished = false;
};
}
