#include "pch.h"
#include "RendererController.h"

#include "D3D12PathTracingBackend.h"

#include <microsoft.ui.xaml.media.dxinterop.h>

using Microsoft::WRL::ComPtr;

namespace lookdevpt::winui
{
namespace
{
std::vector<std::wstring> CommandLineArguments()
{
    int count = 0;
    wchar_t** raw = CommandLineToArgvW(GetCommandLineW(), &count);
    std::vector<std::wstring> result;
    if (raw)
    {
        result.assign(raw, raw + count);
        LocalFree(raw);
    }
    return result;
}
}

RendererController::RendererController()
{
    auto initial = std::make_shared<EditorSnapshot>();
    initial->status = L"Renderer thread starting";
    m_snapshot.Publish(initial);
}

RendererController::~RendererController()
{
    StopAndJoin();
}

void RendererController::Start()
{
    bool expected = false;
    if (!m_started.compare_exchange_strong(expected, true))
    {
        return;
    }
    {
        std::lock_guard lock(m_stateMutex);
        m_renderLoopStopped = false;
        m_viewportDetached = true;
        m_threadFinished = false;
    }
    m_renderThread = std::jthread([this](std::stop_token token)
    {
        RenderMain(token);
    });
}

void RendererController::RequestRenderStopAndWait() noexcept
{
    if (!m_started.load(std::memory_order_acquire))
    {
        return;
    }
    if (m_renderThread.joinable())
    {
        m_renderThread.request_stop();
        std::unique_lock lock(m_stateMutex);
        m_shutdownCondition.wait(lock, [this]
        {
            return m_renderLoopStopped || m_threadFinished;
        });
    }
}

void RendererController::StopAndJoin() noexcept
{
    if (!m_started.exchange(false))
    {
        return;
    }
    if (m_renderThread.joinable())
    {
        m_renderThread.request_stop();
        {
            std::lock_guard lock(m_stateMutex);
            // Destructor/error fallback. The normal UI path calls
            // DetachViewport before reaching this point.
            m_viewportDetached = true;
        }
        m_shutdownCondition.notify_all();
        m_renderThread.join();
    }
}

void RendererController::Enqueue(EditorCommand command)
{
    std::lock_guard lock(m_stateMutex);
    m_commands.Enqueue(std::move(command));
}

void RendererController::SetViewportFocused(bool focused)
{
    m_viewportFocused.store(focused, std::memory_order_release);
    Enqueue({
        .type = EditorCommandType::SetValue,
        .property = L"viewport.focused",
        .value = focused,
    });
}

EditorSnapshotPtr RendererController::LatestSnapshot() const noexcept
{
    return m_snapshot.Latest();
}

bool RendererController::AttachViewport(
    ISwapChainPanelNative* panel,
    std::wstring& diagnostics)
{
    if (!panel)
    {
        diagnostics = L"SwapChainPanel native interface is unavailable.";
        return false;
    }

    ComPtr<IDXGISwapChain3> swapChain;
    {
        std::lock_guard lock(m_stateMutex);
        swapChain = m_swapChain;
    }
    if (!swapChain)
    {
        diagnostics = L"Renderer is still creating the composition swap chain.";
        return false;
    }
    const HRESULT result = panel->SetSwapChain(swapChain.Get());
    if (FAILED(result))
    {
        diagnostics = L"ISwapChainPanelNative::SetSwapChain failed (0x" +
            std::to_wstring(static_cast<unsigned long>(result)) + L").";
        return false;
    }
    {
        std::lock_guard lock(m_stateMutex);
        m_viewportDetached = false;
    }
    diagnostics.clear();
    return true;
}

void RendererController::DetachViewport(
    ISwapChainPanelNative* panel,
    std::wstring& diagnostics) noexcept
{
    diagnostics.clear();
    if (!panel)
    {
        {
            std::lock_guard lock(m_stateMutex);
            m_viewportDetached = true;
        }
        m_shutdownCondition.notify_all();
        return;
    }
    const HRESULT result = panel->SetSwapChain(nullptr);
    if (FAILED(result))
    {
        diagnostics = L"Failed to detach the composition swap chain.";
    }
    {
        std::lock_guard lock(m_stateMutex);
        m_viewportDetached = true;
    }
    m_shutdownCondition.notify_all();
}

void RendererController::RenderMain(std::stop_token stopToken) noexcept
{
    try
    {
        D3D12PathTracingBackend renderer(
            1920,
            1080,
            L"D3D12LookDevPTwithAI",
            PathTracingMode::ReSTIRCombined);

        std::vector<std::wstring> arguments = CommandLineArguments();
        std::vector<wchar_t*> pointers;
        pointers.reserve(arguments.size());
        for (std::wstring& argument : arguments)
        {
            pointers.push_back(argument.data());
        }
        renderer.ParseCommandLineArgs(
            pointers.data(),
            static_cast<int>(pointers.size()));
        renderer.OnInit();

        {
            std::lock_guard lock(m_stateMutex);
            m_swapChain = renderer.GetCompositionSwapChain();
        }
        Publish(renderer);
        auto nextSnapshot = std::chrono::steady_clock::now();

        while (!stopToken.stop_requested() &&
               !renderer.IsBenchmarkFinished())
        {
            renderer.SetViewportFocused(
                m_viewportFocused.load(std::memory_order_acquire));
            DrainCommands(renderer);
            renderer.OnUpdate();
            renderer.OnRender();
            const auto now = std::chrono::steady_clock::now();
            if (now >= nextSnapshot)
            {
                Publish(renderer);
                nextSnapshot = now + std::chrono::milliseconds(100);
            }
        }

        const bool benchmarkFinished =
            renderer.IsBenchmarkFinished();
        auto terminal = std::make_shared<EditorSnapshot>(
            renderer.CaptureEditorSnapshot());
        terminal->rendererReady = false;
        terminal->rendererStopped = true;
        terminal->benchmarkFinished = benchmarkFinished;
        terminal->status = benchmarkFinished
            ? L"Benchmark complete"
            : L"Renderer stopped";
        const EditorSnapshotPtr previous =
            m_snapshot.Latest();
        terminal->revision = previous
            ? previous->revision + 1
            : 1;
        m_snapshot.Publish(terminal);

        {
            std::unique_lock lock(m_stateMutex);
            m_renderLoopStopped = true;
            m_shutdownCondition.notify_all();
            m_shutdownCondition.wait(lock, [this]
            {
                return m_viewportDetached;
            });
        }

        // The UI thread has detached the composition swap chain. OnDestroy
        // waits for the GPU before releasing D3D12 resources.
        renderer.OnDestroy();
        {
            std::lock_guard lock(m_stateMutex);
            m_swapChain.Reset();
            m_threadFinished = true;
        }
        m_shutdownCondition.notify_all();
    }
    catch (winrt::hresult_error const& error)
    {
        PublishFailure(error.message().c_str());
    }
    catch (std::exception const& error)
    {
        PublishFailure(winrt::to_hstring(error.what()).c_str());
    }
    catch (...)
    {
        PublishFailure(L"Unknown renderer-thread failure.");
    }
}

void RendererController::DrainCommands(D3D12PathTracingBackend& renderer)
{
    const auto started = std::chrono::steady_clock::now();
    std::deque<EditorCommand> pending;
    {
        std::lock_guard lock(m_stateMutex);
        pending = m_commands.TakeAll();
    }
    for (EditorCommand const& command : pending)
    {
        renderer.ApplyEditorCommand(command);
    }
    renderer.AddEditorCpuTime(
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - started).count());
}

void RendererController::Publish(D3D12PathTracingBackend& renderer)
{
    const auto started = std::chrono::steady_clock::now();
    auto snapshot = std::make_shared<EditorSnapshot>(
        renderer.CaptureEditorSnapshot());
    const EditorSnapshotPtr previous =
        m_snapshot.Latest();
    snapshot->revision = previous ? previous->revision + 1 : 1;
    snapshot->rendererReady = true;
    m_snapshot.Publish(std::move(snapshot));
    renderer.AddEditorCpuTime(
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - started).count());
}

void RendererController::PublishFailure(std::wstring diagnostics) noexcept
{
    auto failed = std::make_shared<EditorSnapshot>();
    failed->rendererStopped = true;
    failed->status = L"Renderer failed";
    failed->diagnostics = std::move(diagnostics);
    const EditorSnapshotPtr previous =
        m_snapshot.Latest();
    failed->revision = previous ? previous->revision + 1 : 1;
    m_snapshot.Publish(std::move(failed));
    {
        std::lock_guard lock(m_stateMutex);
        m_renderLoopStopped = true;
        m_threadFinished = true;
    }
    m_shutdownCondition.notify_all();
}
}
