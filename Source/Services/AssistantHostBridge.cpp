#include "AssistantHostBridge.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif

#include <windows.h>
#include <objbase.h>
#include <sddl.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <limits>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace lookdevpt::assistant
{
namespace
{
constexpr DWORD PipeBufferBytes = 64u * 1024u;
constexpr DWORD GracefulShutdownWaitMilliseconds = 2'000u;
constexpr UINT ForcedChildExitCode = 0x41534953u; // "ASIS"

class UniqueHandle final
{
public:
    UniqueHandle() = default;
    explicit UniqueHandle(HANDLE value) noexcept : m_value(value) {}
    ~UniqueHandle() { Reset(); }

    UniqueHandle(UniqueHandle const&) = delete;
    UniqueHandle& operator=(UniqueHandle const&) = delete;

    UniqueHandle(UniqueHandle&& other) noexcept
        : m_value(other.Release())
    {
    }

    UniqueHandle& operator=(UniqueHandle&& other) noexcept
    {
        if (this != &other)
        {
            Reset(other.Release());
        }
        return *this;
    }

    [[nodiscard]] HANDLE Get() const noexcept { return m_value; }
    [[nodiscard]] explicit operator bool() const noexcept
    {
        return m_value && m_value != INVALID_HANDLE_VALUE;
    }

    HANDLE Release() noexcept
    {
        const HANDLE value = m_value;
        m_value = nullptr;
        return value;
    }

    void Reset(HANDLE value = nullptr) noexcept
    {
        if (*this)
        {
            CloseHandle(m_value);
        }
        m_value = value;
    }

private:
    HANDLE m_value = nullptr;
};

class LocalAllocation final
{
public:
    LocalAllocation() = default;
    explicit LocalAllocation(void* value) noexcept : m_value(value) {}
    ~LocalAllocation() { Reset(); }

    LocalAllocation(LocalAllocation const&) = delete;
    LocalAllocation& operator=(LocalAllocation const&) = delete;

    LocalAllocation(LocalAllocation&& other) noexcept
        : m_value(other.Release())
    {
    }

    LocalAllocation& operator=(LocalAllocation&& other) noexcept
    {
        if (this != &other)
        {
            Reset(other.Release());
        }
        return *this;
    }

    [[nodiscard]] void* Get() const noexcept { return m_value; }

private:
    void* Release() noexcept
    {
        void* value = m_value;
        m_value = nullptr;
        return value;
    }

    void Reset(void* value = nullptr) noexcept
    {
        if (m_value)
        {
            LocalFree(m_value);
        }
        m_value = value;
    }

    void* m_value = nullptr;
};

[[noreturn]] void ThrowWindowsError(char const* operation)
{
    throw std::system_error(
        static_cast<int>(GetLastError()),
        std::system_category(),
        operation);
}

std::string SafeWindowsDiagnostic(
    char const* operation,
    DWORD error)
{
    std::string result(operation);
    if (error != ERROR_SUCCESS)
    {
        result += " (Windows error ";
        result += std::to_string(error);
        result += ").";
    }
    return result;
}

UniqueHandle CreateManualResetEvent()
{
    UniqueHandle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!event)
    {
        ThrowWindowsError("Could not create a bridge event");
    }
    return event;
}

LocalAllocation CreateCurrentUserSecurityDescriptor()
{
    UniqueHandle token;
    HANDLE rawToken = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &rawToken))
    {
        ThrowWindowsError("Could not query the current process token");
    }
    token.Reset(rawToken);

    DWORD tokenBytes = 0;
    if (GetTokenInformation(
            token.Get(), TokenUser, nullptr, 0, &tokenBytes) ||
        GetLastError() != ERROR_INSUFFICIENT_BUFFER ||
        tokenBytes < sizeof(TOKEN_USER))
    {
        ThrowWindowsError("Could not size the current user token");
    }

    std::vector<std::byte> tokenBuffer(tokenBytes);
    if (!GetTokenInformation(
            token.Get(),
            TokenUser,
            tokenBuffer.data(),
            tokenBytes,
            &tokenBytes))
    {
        ThrowWindowsError("Could not read the current user token");
    }

    auto const* tokenUser =
        reinterpret_cast<TOKEN_USER const*>(tokenBuffer.data());
    if (!IsValidSid(tokenUser->User.Sid))
    {
        SetLastError(ERROR_INVALID_SID);
        ThrowWindowsError("The current user SID is invalid");
    }

    LPWSTR rawSidText = nullptr;
    if (!ConvertSidToStringSidW(tokenUser->User.Sid, &rawSidText))
    {
        ThrowWindowsError("Could not format the current user SID");
    }
    LocalAllocation sidText(rawSidText);

    const std::wstring sddl =
        L"D:P(A;;GA;;;" +
        std::wstring(static_cast<wchar_t const*>(sidText.Get())) +
        L")";
    PSECURITY_DESCRIPTOR rawDescriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            sddl.c_str(),
            SDDL_REVISION_1,
            &rawDescriptor,
            nullptr))
    {
        ThrowWindowsError(
            "Could not create the current-user pipe descriptor");
    }
    return LocalAllocation(rawDescriptor);
}

std::filesystem::path ExecutableDirectory()
{
    std::wstring path(32768, L'\0');
    const DWORD length = GetModuleFileNameW(
        nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0 || length >= path.size())
    {
        ThrowWindowsError("Could not locate the native executable");
    }
    path.resize(length);
    return std::filesystem::path(path).parent_path();
}

std::filesystem::path ResolveChatHostExecutable(
    std::filesystem::path path)
{
    const std::filesystem::path directory = ExecutableDirectory();
    if (path.empty())
    {
        path = directory /
            L"D3D12LookDevPTwithAI.ChatHost.exe";
    }
    else if (path.is_relative())
    {
        path = directory / path;
    }
    return std::filesystem::absolute(path).lexically_normal();
}

std::wstring MakeUniquePipeName()
{
    GUID identifier{};
    if (FAILED(CoCreateGuid(&identifier)))
    {
        throw std::runtime_error(
            "Could not create a unique ChatHost pipe name.");
    }

    std::array<wchar_t, 40> text{};
    if (StringFromGUID2(
            identifier, text.data(), static_cast<int>(text.size())) <= 0)
    {
        throw std::runtime_error(
            "Could not format a unique ChatHost pipe name.");
    }
    std::wstring suffix(text.data());
    std::erase(suffix, L'{');
    std::erase(suffix, L'}');
    return L"D3D12LookDevPTwithAI.ChatHost." +
        std::to_wstring(GetCurrentProcessId()) + L"." + suffix;
}

std::wstring QuoteCommandLineArgument(std::wstring_view argument)
{
    std::wstring result;
    result.push_back(L'"');
    std::size_t backslashes = 0;
    for (wchar_t character : argument)
    {
        if (character == L'\\')
        {
            ++backslashes;
            continue;
        }
        if (character == L'"')
        {
            result.append(backslashes * 2u + 1u, L'\\');
            result.push_back(character);
            backslashes = 0;
            continue;
        }
        result.append(backslashes, L'\\');
        backslashes = 0;
        result.push_back(character);
    }
    result.append(backslashes * 2u, L'\\');
    result.push_back(L'"');
    return result;
}

UniqueHandle CreatePipe(std::wstring const& simpleName)
{
    LocalAllocation descriptor =
        CreateCurrentUserSecurityDescriptor();
    SECURITY_ATTRIBUTES security{};
    security.nLength = sizeof(security);
    security.lpSecurityDescriptor = descriptor.Get();
    security.bInheritHandle = FALSE;

    const std::wstring fullName = L"\\\\.\\pipe\\" + simpleName;
    UniqueHandle pipe(CreateNamedPipeW(
        fullName.c_str(),
        PIPE_ACCESS_DUPLEX |
            FILE_FLAG_OVERLAPPED |
            FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_BYTE |
            PIPE_READMODE_BYTE |
            PIPE_WAIT |
            PIPE_REJECT_REMOTE_CLIENTS,
        1,
        PipeBufferBytes,
        PipeBufferBytes,
        0,
        &security));
    if (!pipe)
    {
        ThrowWindowsError("Could not create the ChatHost pipe");
    }
    return pipe;
}

UniqueHandle CreateKillOnCloseJob()
{
    UniqueHandle job(CreateJobObjectW(nullptr, nullptr));
    if (!job)
    {
        ThrowWindowsError("Could not create the ChatHost job");
    }

    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags =
        JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(
            job.Get(),
            JobObjectExtendedLimitInformation,
            &limits,
            sizeof(limits)))
    {
        ThrowWindowsError("Could not configure the ChatHost job");
    }
    return job;
}

struct ChildProcess
{
    UniqueHandle process;
    DWORD processId = 0;
};

ChildProcess LaunchChatHost(
    std::filesystem::path const& executable,
    std::wstring const& pipeName,
    HANDLE job)
{
    std::wstring commandLine =
        QuoteCommandLineArgument(executable.native()) +
        L" --pipe-name " + QuoteCommandLineArgument(pipeName) +
        L" --parent-pid " +
        std::to_wstring(GetCurrentProcessId());
    std::vector<wchar_t> mutableCommandLine(
        commandLine.begin(), commandLine.end());
    mutableCommandLine.push_back(L'\0');

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION processInfo{};
    const std::wstring workingDirectory =
        executable.parent_path().native();
    if (!CreateProcessW(
            executable.c_str(),
            mutableCommandLine.data(),
            nullptr,
            nullptr,
            FALSE,
            CREATE_NO_WINDOW |
                CREATE_SUSPENDED |
                CREATE_UNICODE_ENVIRONMENT,
            nullptr,
            workingDirectory.empty()
                ? nullptr
                : workingDirectory.c_str(),
            &startup,
            &processInfo))
    {
        ThrowWindowsError("Could not launch ChatHost");
    }

    UniqueHandle process(processInfo.hProcess);
    UniqueHandle thread(processInfo.hThread);
    if (!AssignProcessToJobObject(job, process.Get()))
    {
        const DWORD error = GetLastError();
        TerminateProcess(process.Get(), ForcedChildExitCode);
        SetLastError(error);
        ThrowWindowsError("Could not assign ChatHost to its job");
    }
    if (ResumeThread(thread.Get()) == static_cast<DWORD>(-1))
    {
        const DWORD error = GetLastError();
        TerminateProcess(process.Get(), ForcedChildExitCode);
        SetLastError(error);
        ThrowWindowsError("Could not resume ChatHost");
    }

    ChildProcess result;
    result.process = std::move(process);
    result.processId = processInfo.dwProcessId;
    return result;
}

void CancelAndDrain(
    HANDLE pipe,
    OVERLAPPED& operation) noexcept
{
    CancelIoEx(pipe, &operation);
    WaitForSingleObject(operation.hEvent, INFINITE);
    DWORD ignored = 0;
    GetOverlappedResult(pipe, &operation, &ignored, FALSE);
}

enum class ConnectStatus
{
    Connected,
    Stopped,
    ChildExited,
    TimedOut,
    Failed,
};

struct ConnectOutcome
{
    ConnectStatus status = ConnectStatus::Failed;
    DWORD error = ERROR_SUCCESS;
};

ConnectOutcome ConnectToChild(
    HANDLE pipe,
    HANDLE child,
    HANDLE stopEvent,
    DWORD timeoutMilliseconds)
{
    UniqueHandle completed = CreateManualResetEvent();
    OVERLAPPED operation{};
    operation.hEvent = completed.Get();
    if (ConnectNamedPipe(pipe, &operation))
    {
        return { ConnectStatus::Connected, ERROR_SUCCESS };
    }

    const DWORD connectError = GetLastError();
    if (connectError == ERROR_PIPE_CONNECTED)
    {
        return { ConnectStatus::Connected, ERROR_SUCCESS };
    }
    if (connectError != ERROR_IO_PENDING)
    {
        return { ConnectStatus::Failed, connectError };
    }

    const std::array<HANDLE, 3> waits = {
        stopEvent,
        child,
        completed.Get(),
    };
    const DWORD wait = WaitForMultipleObjects(
        static_cast<DWORD>(waits.size()),
        waits.data(),
        FALSE,
        timeoutMilliseconds);
    if (wait == WAIT_OBJECT_0 + 2u)
    {
        DWORD ignored = 0;
        if (!GetOverlappedResult(pipe, &operation, &ignored, FALSE))
        {
            return { ConnectStatus::Failed, GetLastError() };
        }
        return { ConnectStatus::Connected, ERROR_SUCCESS };
    }

    CancelAndDrain(pipe, operation);
    if (wait == WAIT_OBJECT_0)
    {
        return { ConnectStatus::Stopped, ERROR_OPERATION_ABORTED };
    }
    if (wait == WAIT_OBJECT_0 + 1u)
    {
        return { ConnectStatus::ChildExited, ERROR_BROKEN_PIPE };
    }
    if (wait == WAIT_TIMEOUT)
    {
        return { ConnectStatus::TimedOut, WAIT_TIMEOUT };
    }
    return { ConnectStatus::Failed, GetLastError() };
}

enum class IoStatus
{
    Completed,
    Stopped,
    Disconnected,
    Failed,
};

struct IoOutcome
{
    IoStatus status = IoStatus::Failed;
    DWORD bytes = 0;
    DWORD error = ERROR_SUCCESS;
};

bool IsDisconnectedError(DWORD error) noexcept
{
    return error == ERROR_BROKEN_PIPE ||
        error == ERROR_PIPE_NOT_CONNECTED ||
        error == ERROR_NO_DATA;
}

IoOutcome RunPipeIo(
    HANDLE pipe,
    HANDLE stopEvent,
    HANDLE completionEvent,
    void* buffer,
    DWORD bytes,
    bool write)
{
    if (WaitForSingleObject(stopEvent, 0) == WAIT_OBJECT_0)
    {
        return { IoStatus::Stopped, 0, ERROR_OPERATION_ABORTED };
    }

    ResetEvent(completionEvent);
    OVERLAPPED operation{};
    operation.hEvent = completionEvent;
    DWORD transferred = 0;
    const BOOL started = write
        ? WriteFile(
            pipe, buffer, bytes, &transferred, &operation)
        : ReadFile(
            pipe, buffer, bytes, &transferred, &operation);
    if (started)
    {
        if (transferred == 0)
        {
            return { IoStatus::Disconnected, 0, ERROR_BROKEN_PIPE };
        }
        return { IoStatus::Completed, transferred, ERROR_SUCCESS };
    }

    const DWORD startError = GetLastError();
    if (startError != ERROR_IO_PENDING)
    {
        return {
            IsDisconnectedError(startError)
                ? IoStatus::Disconnected
                : IoStatus::Failed,
            0,
            startError,
        };
    }

    const std::array<HANDLE, 2> waits = {
        stopEvent,
        completionEvent,
    };
    const DWORD wait = WaitForMultipleObjects(
        static_cast<DWORD>(waits.size()),
        waits.data(),
        FALSE,
        INFINITE);
    if (wait == WAIT_OBJECT_0)
    {
        CancelAndDrain(pipe, operation);
        return { IoStatus::Stopped, 0, ERROR_OPERATION_ABORTED };
    }
    if (wait != WAIT_OBJECT_0 + 1u)
    {
        const DWORD error = GetLastError();
        CancelAndDrain(pipe, operation);
        return { IoStatus::Failed, 0, error };
    }

    if (!GetOverlappedResult(
            pipe, &operation, &transferred, FALSE))
    {
        const DWORD error = GetLastError();
        return {
            IsDisconnectedError(error)
                ? IoStatus::Disconnected
                : IoStatus::Failed,
            0,
            error,
        };
    }
    if (transferred == 0)
    {
        return { IoStatus::Disconnected, 0, ERROR_BROKEN_PIPE };
    }
    return { IoStatus::Completed, transferred, ERROR_SUCCESS };
}
}

class AssistantHostBridge::Impl final
{
public:
    void Start(
        AssistantHostBridgeOptions options,
        AssistantEventSink eventSink,
        AssistantHostStateSink stateSink)
    {
        ValidateOptions(options);
        std::lock_guard lifecycleLock(m_lifecycleMutex);
        if (m_started)
        {
            throw std::logic_error(
                "AssistantHostBridge is already started.");
        }

        m_options = std::move(options);
        m_eventSink = std::move(eventSink);
        m_stateSink = std::move(stateSink);
        PublishState(
            AssistantHostBridgeState::Starting,
            "Starting local ChatHost.");

        try
        {
            const std::filesystem::path executable =
                ResolveChatHostExecutable(
                    m_options.chatHostExecutable);
            std::error_code fileError;
            if (!std::filesystem::is_regular_file(
                    executable, fileError))
            {
                throw std::system_error(
                    fileError
                        ? fileError
                        : std::make_error_code(
                            std::errc::no_such_file_or_directory),
                    "ChatHost executable is unavailable");
            }

            m_stopEvent = CreateManualResetEvent();
            const std::wstring pipeName = MakeUniquePipeName();
            m_pipe = CreatePipe(pipeName);
            m_job = CreateKillOnCloseJob();
            ChildProcess child = LaunchChatHost(
                executable, pipeName, m_job.Get());
            m_childProcess = std::move(child.process);
            m_childProcessId = child.processId;

            {
                std::lock_guard queueLock(m_queueMutex);
                m_queue.clear();
                m_queuedBytes = 0;
                m_lastPostedSequence.reset();
            }
            m_failed.store(false, std::memory_order_release);
            m_connected.store(false, std::memory_order_release);
            m_shutdownPosted.store(false, std::memory_order_release);
            m_acceptingPosts.store(true, std::memory_order_release);
            m_started = true;

            m_writerThread = std::jthread(
                [this](std::stop_token token)
                {
                    WriterMain(token);
                });
            m_readerThread = std::jthread(
                [this](std::stop_token token)
                {
                    ReaderMain(token);
                });
        }
        catch (std::system_error const& error)
        {
            CleanupFailedStart();
            PublishState(
                AssistantHostBridgeState::Failed,
                SafeWindowsDiagnostic(
                    "ChatHost bridge startup failed",
                    static_cast<DWORD>(error.code().value())));
            m_eventSink = {};
            m_stateSink = {};
            throw;
        }
        catch (...)
        {
            CleanupFailedStart();
            PublishState(
                AssistantHostBridgeState::Failed,
                "ChatHost bridge startup failed.");
            m_eventSink = {};
            m_stateSink = {};
            throw;
        }
    }

    [[nodiscard]] bool Post(std::string_view jsonEnvelope)
    {
        if (!m_acceptingPosts.load(std::memory_order_acquire))
        {
            return false;
        }

        AssistantEnvelope envelope = ParseEnvelope(jsonEnvelope);
        auto frame = std::make_shared<std::vector<std::uint8_t>>(
            EncodeFrame(jsonEnvelope));
        const bool isShutdown =
            cld::JsonStringOr(envelope.root, "kind") == "request" &&
            cld::JsonStringOr(envelope.root, "method") == "shutdown";

        {
            std::lock_guard queueLock(m_queueMutex);
            if (!m_acceptingPosts.load(std::memory_order_relaxed))
            {
                return false;
            }
            if (m_lastPostedSequence &&
                envelope.sequence <= *m_lastPostedSequence)
            {
                throw ProtocolError(
                    ProtocolErrorCode::NonMonotonicSequence,
                    "Assistant outgoing sequence is not increasing.");
            }
            if (m_queue.size() >=
                    m_options.maximumQueuedFrames ||
                frame->size() >
                    m_options.maximumQueuedBytes ||
                m_queuedBytes >
                    m_options.maximumQueuedBytes - frame->size())
            {
                return false;
            }

            m_queue.emplace_back(std::move(frame));
            m_queuedBytes += m_queue.back()->size();
            m_lastPostedSequence = envelope.sequence;
            if (isShutdown)
            {
                m_shutdownPosted.store(
                    true, std::memory_order_release);
                m_acceptingPosts.store(
                    false, std::memory_order_release);
            }
        }
        m_queueCondition.notify_one();
        return true;
    }

    void StopAndJoin() noexcept
    {
        std::lock_guard lifecycleLock(m_lifecycleMutex);
        if (!m_started)
        {
            return;
        }

        m_acceptingPosts.store(false, std::memory_order_release);
        bool childExited = false;
        if (m_shutdownPosted.load(std::memory_order_acquire) &&
            m_childProcess)
        {
            childExited = WaitForSingleObject(
                m_childProcess.Get(),
                GracefulShutdownWaitMilliseconds) == WAIT_OBJECT_0;
        }
        if (m_stopEvent)
        {
            SetEvent(m_stopEvent.Get());
        }
        if (m_readerThread.joinable())
        {
            m_readerThread.request_stop();
        }
        if (m_writerThread.joinable())
        {
            m_writerThread.request_stop();
        }
        m_queueCondition.notify_all();
        if (m_pipe)
        {
            CancelIoEx(m_pipe.Get(), nullptr);
        }
        if (m_job && !childExited)
        {
            TerminateJobObject(m_job.Get(), ForcedChildExitCode);
        }

        if (m_readerThread.joinable())
        {
            m_readerThread.join();
        }
        if (m_writerThread.joinable())
        {
            m_writerThread.join();
        }

        std::optional<std::uint32_t> childExitCode;
        if (m_childProcess &&
            WaitForSingleObject(m_childProcess.Get(), 0) == WAIT_OBJECT_0)
        {
            DWORD value = 0;
            if (GetExitCodeProcess(m_childProcess.Get(), &value))
            {
                childExitCode = static_cast<std::uint32_t>(value);
            }
        }

        if (m_pipe)
        {
            DisconnectNamedPipe(m_pipe.Get());
        }
        m_connected.store(false, std::memory_order_release);
        m_failed.store(false, std::memory_order_release);
        m_shutdownPosted.store(false, std::memory_order_release);
        m_childProcessId = 0;
        m_childProcess.Reset();
        m_pipe.Reset();
        m_job.Reset();
        m_stopEvent.Reset();
        {
            std::lock_guard queueLock(m_queueMutex);
            m_queue.clear();
            m_queuedBytes = 0;
            m_lastPostedSequence.reset();
        }
        m_started = false;
        PublishState(
            AssistantHostBridgeState::Stopped,
            "ChatHost stopped.",
            childExitCode);
        m_eventSink = {};
        m_stateSink = {};
    }

private:
    using Frame = std::shared_ptr<std::vector<std::uint8_t>>;

    static void ValidateOptions(
        AssistantHostBridgeOptions const& options)
    {
        if (options.connectionTimeout.count() <= 0 ||
            options.connectionTimeout.count() >
                static_cast<long long>(
                    (std::numeric_limits<DWORD>::max)() - 1u))
        {
            throw std::invalid_argument(
                "Assistant connection timeout is invalid.");
        }
        if (options.maximumQueuedFrames == 0 ||
            options.maximumQueuedBytes == 0)
        {
            throw std::invalid_argument(
                "Assistant queue bounds must be positive.");
        }
    }

    void PublishState(
        AssistantHostBridgeState state,
        std::string diagnostic,
        std::optional<std::uint32_t> childExitCode = std::nullopt) noexcept
    {
        if (!m_stateSink)
        {
            return;
        }
        try
        {
            m_stateSink({
                state,
                std::move(diagnostic),
                childExitCode,
            });
        }
        catch (...)
        {
            // A callback cannot be allowed to tear down the transport thread.
        }
    }

    void Fail(char const* operation, DWORD error) noexcept
    {
        bool expected = false;
        if (!m_failed.compare_exchange_strong(
                expected, true, std::memory_order_acq_rel))
        {
            return;
        }

        m_acceptingPosts.store(false, std::memory_order_release);
        m_connected.store(false, std::memory_order_release);
        if (m_stopEvent)
        {
            SetEvent(m_stopEvent.Get());
        }
        if (m_pipe)
        {
            CancelIoEx(m_pipe.Get(), nullptr);
        }
        if (m_job)
        {
            TerminateJobObject(m_job.Get(), ForcedChildExitCode);
        }
        m_queueCondition.notify_all();
        PublishState(
            AssistantHostBridgeState::Failed,
            SafeWindowsDiagnostic(operation, error));
    }

    void ReaderMain(std::stop_token token) noexcept
    {
        try
        {
            PublishState(
                AssistantHostBridgeState::WaitingForConnection,
                "Waiting for local ChatHost.");
            const ConnectOutcome connected = ConnectToChild(
                m_pipe.Get(),
                m_childProcess.Get(),
                m_stopEvent.Get(),
                static_cast<DWORD>(
                    m_options.connectionTimeout.count()));
            if (token.stop_requested() ||
                connected.status == ConnectStatus::Stopped)
            {
                return;
            }
            if (connected.status == ConnectStatus::TimedOut)
            {
                Fail("ChatHost connection timed out", connected.error);
                return;
            }
            if (connected.status != ConnectStatus::Connected)
            {
                Fail("ChatHost could not connect", connected.error);
                return;
            }

            ULONG clientProcessId = 0;
            if (!GetNamedPipeClientProcessId(
                    m_pipe.Get(), &clientProcessId))
            {
                Fail(
                    "ChatHost client identity check failed",
                    GetLastError());
                return;
            }
            if (clientProcessId != m_childProcessId)
            {
                Fail(
                    "An unexpected process connected to ChatHost IPC",
                    ERROR_ACCESS_DENIED);
                return;
            }

            m_connected.store(true, std::memory_order_release);
            PublishState(
                AssistantHostBridgeState::Connected,
                "Local ChatHost connected.");
            m_queueCondition.notify_all();

            UniqueHandle completion = CreateManualResetEvent();
            std::array<std::uint8_t, PipeBufferBytes> buffer{};
            IncrementalFrameDecoder decoder;
            while (!token.stop_requested())
            {
                const IoOutcome read = RunPipeIo(
                    m_pipe.Get(),
                    m_stopEvent.Get(),
                    completion.Get(),
                    buffer.data(),
                    static_cast<DWORD>(buffer.size()),
                    false);
                if (read.status == IoStatus::Stopped)
                {
                    return;
                }
                if (read.status == IoStatus::Disconnected)
                {
                    try
                    {
                        decoder.Finish();
                    }
                    catch (ProtocolError const&)
                    {
                        Fail(
                            "ChatHost closed with a partial frame",
                            ERROR_INVALID_DATA);
                        return;
                    }
                    if (m_shutdownPosted.load(
                            std::memory_order_acquire))
                    {
                        m_acceptingPosts.store(
                            false, std::memory_order_release);
                        m_connected.store(
                            false, std::memory_order_release);
                        SetEvent(m_stopEvent.Get());
                        m_queueCondition.notify_all();
                        PublishState(
                            AssistantHostBridgeState::Stopped,
                            "ChatHost completed shutdown.");
                        return;
                    }
                    Fail(
                        "ChatHost disconnected unexpectedly",
                        read.error);
                    return;
                }
                if (read.status != IoStatus::Completed)
                {
                    Fail("ChatHost read failed", read.error);
                    return;
                }

                std::vector<AssistantEnvelope> envelopes;
                try
                {
                    envelopes = decoder.Push(
                        std::span<std::uint8_t const>(
                            buffer.data(), read.bytes));
                }
                catch (ProtocolError const&)
                {
                    Fail(
                        "ChatHost sent an invalid protocol frame",
                        ERROR_INVALID_DATA);
                    return;
                }
                for (AssistantEnvelope& envelope : envelopes)
                {
                    if (!m_eventSink)
                    {
                        continue;
                    }
                    try
                    {
                        m_eventSink(std::move(envelope));
                    }
                    catch (...)
                    {
                        // UI dispatch failures do not expose or corrupt IPC.
                    }
                }
            }
        }
        catch (std::system_error const& error)
        {
            if (!token.stop_requested())
            {
                Fail(
                    "ChatHost reader initialization failed",
                    static_cast<DWORD>(error.code().value()));
            }
        }
        catch (...)
        {
            if (!token.stop_requested())
            {
                Fail(
                    "ChatHost reader failed",
                    ERROR_UNHANDLED_EXCEPTION);
            }
        }
    }

    void WriterMain(std::stop_token token) noexcept
    {
        try
        {
            UniqueHandle completion = CreateManualResetEvent();
            while (!token.stop_requested())
            {
                Frame frame;
                {
                    std::unique_lock queueLock(m_queueMutex);
                    m_queueCondition.wait(queueLock, [this, &token]
                    {
                        return token.stop_requested() ||
                            m_failed.load(
                                std::memory_order_acquire) ||
                            (m_connected.load(
                                std::memory_order_acquire) &&
                             !m_queue.empty());
                    });
                    if (token.stop_requested() ||
                        m_failed.load(std::memory_order_acquire))
                    {
                        return;
                    }
                    frame = m_queue.front();
                }

                std::size_t offset = 0;
                while (offset < frame->size())
                {
                    const std::size_t remaining =
                        frame->size() - offset;
                    const DWORD chunk = static_cast<DWORD>(
                        (std::min)(
                            remaining,
                            static_cast<std::size_t>(
                                (std::numeric_limits<DWORD>::max)())));
                    const IoOutcome written = RunPipeIo(
                        m_pipe.Get(),
                        m_stopEvent.Get(),
                        completion.Get(),
                        frame->data() + offset,
                        chunk,
                        true);
                    if (written.status == IoStatus::Stopped)
                    {
                        return;
                    }
                    if (written.status != IoStatus::Completed)
                    {
                        Fail("ChatHost write failed", written.error);
                        return;
                    }
                    offset += written.bytes;
                }

                {
                    std::lock_guard queueLock(m_queueMutex);
                    if (!m_queue.empty() &&
                        m_queue.front() == frame)
                    {
                        m_queuedBytes -= frame->size();
                        m_queue.pop_front();
                    }
                }
            }
        }
        catch (std::system_error const& error)
        {
            if (!token.stop_requested())
            {
                Fail(
                    "ChatHost writer initialization failed",
                    static_cast<DWORD>(error.code().value()));
            }
        }
        catch (...)
        {
            if (!token.stop_requested())
            {
                Fail(
                    "ChatHost writer failed",
                    ERROR_UNHANDLED_EXCEPTION);
            }
        }
    }

    void CleanupFailedStart() noexcept
    {
        m_acceptingPosts.store(false, std::memory_order_release);
        if (m_stopEvent)
        {
            SetEvent(m_stopEvent.Get());
        }
        if (m_readerThread.joinable())
        {
            m_readerThread.request_stop();
        }
        if (m_writerThread.joinable())
        {
            m_writerThread.request_stop();
        }
        m_queueCondition.notify_all();
        if (m_pipe)
        {
            CancelIoEx(m_pipe.Get(), nullptr);
        }
        if (m_job)
        {
            TerminateJobObject(m_job.Get(), ForcedChildExitCode);
        }
        if (m_readerThread.joinable())
        {
            m_readerThread.join();
        }
        if (m_writerThread.joinable())
        {
            m_writerThread.join();
        }
        m_started = false;
        m_childProcessId = 0;
        m_childProcess.Reset();
        m_pipe.Reset();
        m_job.Reset();
        m_stopEvent.Reset();
    }

    std::mutex m_lifecycleMutex;
    std::mutex m_queueMutex;
    std::condition_variable m_queueCondition;
    std::deque<Frame> m_queue;
    std::size_t m_queuedBytes = 0;
    std::optional<std::uint64_t> m_lastPostedSequence;

    AssistantHostBridgeOptions m_options;
    AssistantEventSink m_eventSink;
    AssistantHostStateSink m_stateSink;
    UniqueHandle m_stopEvent;
    UniqueHandle m_pipe;
    UniqueHandle m_job;
    UniqueHandle m_childProcess;
    DWORD m_childProcessId = 0;
    std::jthread m_readerThread;
    std::jthread m_writerThread;
    std::atomic_bool m_acceptingPosts = false;
    std::atomic_bool m_connected = false;
    std::atomic_bool m_failed = false;
    std::atomic_bool m_shutdownPosted = false;
    bool m_started = false;
};

AssistantHostBridge::AssistantHostBridge()
    : m_impl(std::make_unique<Impl>())
{
}

AssistantHostBridge::~AssistantHostBridge()
{
    StopAndJoin();
}

void AssistantHostBridge::Start(
    AssistantHostBridgeOptions options,
    AssistantEventSink eventSink,
    AssistantHostStateSink stateSink)
{
    m_impl->Start(
        std::move(options),
        std::move(eventSink),
        std::move(stateSink));
}

bool AssistantHostBridge::Post(std::string_view jsonEnvelope)
{
    return m_impl->Post(jsonEnvelope);
}

void AssistantHostBridge::StopAndJoin() noexcept
{
    m_impl->StopAndJoin();
}
}
