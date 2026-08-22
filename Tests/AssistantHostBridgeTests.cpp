#include "Services/AssistantHostBridge.h"
#include "SimpleJson.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

using namespace lookdevpt::assistant;

namespace
{
constexpr auto RequestTimeout = std::chrono::seconds(10);

void Require(bool condition, char const* message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

std::string Request(
    std::uint64_t sequence,
    std::string_view requestId,
    std::string_view method,
    std::string_view payload)
{
    return
        "{\"protocolVersion\":1,\"kind\":\"request\","
        "\"requestId\":\"" + cld::EscapeJson(std::string(requestId)) +
        "\",\"sequence\":" + std::to_string(sequence) +
        ",\"method\":\"" + cld::EscapeJson(std::string(method)) +
        "\",\"payload\":" + std::string(payload) + "}";
}

enum class Milestone
{
    Connected,
    Initialized,
    ConversationCreated,
    TurnCompleted,
    ShutdownAcknowledged,
    Stopped,
    ChildExitedCleanly,
};

class Observation final
{
public:
    void OnState(AssistantHostBridgeStateUpdate update)
    {
        {
            std::lock_guard lock(m_mutex);
            if (update.state == AssistantHostBridgeState::Connected)
            {
                m_connected = true;
            }
            else if (update.state == AssistantHostBridgeState::Failed)
            {
                m_failed = true;
            }
            else if (update.state == AssistantHostBridgeState::Stopped)
            {
                m_stopped = true;
                if (update.childExitCode)
                {
                    m_childExitCodeObserved = true;
                    m_childExitCode = *update.childExitCode;
                    if (m_childExitCode != 0)
                    {
                        m_failed = true;
                    }
                }
            }
        }
        m_changed.notify_all();
    }

    void OnEnvelope(AssistantEnvelope envelope) noexcept
    {
        try
        {
            const cld::JsonValue& root = envelope.root;
            const std::string kind = cld::JsonStringOr(root, "kind");
            const std::string method = cld::JsonStringOr(root, "method");
            const cld::JsonValue* payload = cld::FindMember(root, "payload");
            const cld::JsonValue* error = cld::FindMember(root, "error");

            std::lock_guard lock(m_mutex);
            if (kind == "response" && error &&
                error->type != cld::JsonValue::Type::Null)
            {
                m_failed = true;
            }
            else if (!payload ||
                     payload->type != cld::JsonValue::Type::Object)
            {
                m_failed = true;
            }
            else if (kind == "response" && method == "initialize")
            {
                m_activeConversationId = cld::JsonStringOr(
                    *payload, "activeConversationId");
                m_initialized = !m_activeConversationId.empty();
                m_failed = !m_initialized;
            }
            else if (kind == "response" &&
                     method == "conversation.create")
            {
                const cld::JsonValue* conversation =
                    cld::FindMember(*payload, "conversation");
                if (!conversation ||
                    conversation->type != cld::JsonValue::Type::Object)
                {
                    m_failed = true;
                }
                else
                {
                    m_activeConversationId = cld::JsonStringOr(
                        *conversation, "id");
                    m_conversationCreated =
                        !m_activeConversationId.empty();
                    m_failed = !m_conversationCreated;
                }
            }
            else if (kind == "response" && method == "sendTurn")
            {
                m_turnAccepted = cld::JsonBoolOr(
                    *payload, "accepted", false);
                m_failed = !m_turnAccepted;
            }
            else if (kind == "event" && method == "textDelta")
            {
                m_sawTextDelta = !cld::JsonStringOr(
                    *payload, "delta").empty();
            }
            else if (kind == "event" && method == "completed")
            {
                m_turnCompleted =
                    cld::JsonStringOr(*payload, "status") == "completed";
                m_failed = !m_turnCompleted;
            }
            else if (kind == "response" && method == "shutdown")
            {
                m_shutdownAcknowledged = cld::JsonBoolOr(
                    *payload, "accepted", false);
                m_failed = !m_shutdownAcknowledged;
            }
        }
        catch (...)
        {
            std::lock_guard lock(m_mutex);
            m_failed = true;
        }
        m_changed.notify_all();
    }

    bool WaitFor(Milestone milestone)
    {
        std::unique_lock lock(m_mutex);
        const bool signalled = m_changed.wait_for(
            lock,
            RequestTimeout,
            [this, milestone]
            {
                return m_failed || Reached(milestone);
            });
        return signalled && !m_failed && Reached(milestone);
    }

    std::string ActiveConversationId() const
    {
        std::lock_guard lock(m_mutex);
        return m_activeConversationId;
    }

private:
    bool Reached(Milestone milestone) const
    {
        switch (milestone)
        {
        case Milestone::Connected:
            return m_connected;
        case Milestone::Initialized:
            return m_initialized;
        case Milestone::ConversationCreated:
            return m_conversationCreated;
        case Milestone::TurnCompleted:
            return m_turnAccepted && m_sawTextDelta && m_turnCompleted;
        case Milestone::ShutdownAcknowledged:
            return m_shutdownAcknowledged;
        case Milestone::Stopped:
            return m_stopped;
        case Milestone::ChildExitedCleanly:
            return m_childExitCodeObserved && m_childExitCode == 0;
        }
        return false;
    }

    mutable std::mutex m_mutex;
    std::condition_variable m_changed;
    std::string m_activeConversationId;
    bool m_connected = false;
    bool m_initialized = false;
    bool m_conversationCreated = false;
    bool m_turnAccepted = false;
    bool m_sawTextDelta = false;
    bool m_turnCompleted = false;
    bool m_shutdownAcknowledged = false;
    bool m_stopped = false;
    bool m_childExitCodeObserved = false;
    std::uint32_t m_childExitCode = 0;
    bool m_failed = false;
};

void Run(std::filesystem::path const& chatHostExecutable)
{
    Require(
        std::filesystem::is_regular_file(chatHostExecutable),
        "ChatHost executable is unavailable");

    Observation observation;
    AssistantHostBridge bridge;
    AssistantHostBridgeOptions options;
    options.chatHostExecutable = chatHostExecutable;
    options.connectionTimeout = std::chrono::seconds(10);
    options.maximumQueuedFrames = 16;
    options.maximumQueuedBytes = MaximumPayloadBytes + 4u;

    try
    {
        bridge.Start(
            std::move(options),
            [&observation](AssistantEnvelope envelope)
            {
                observation.OnEnvelope(std::move(envelope));
            },
            [&observation](AssistantHostBridgeStateUpdate update)
            {
                observation.OnState(std::move(update));
            });
        Require(
            observation.WaitFor(Milestone::Connected),
            "ChatHost did not connect within the timeout");

        Require(
            bridge.Post(Request(
                1,
                "11111111-1111-1111-1111-111111111111",
                "initialize",
                "{\"instanceId\":\"bridge-e2e-instance\","
                "\"projectContextKey\":\"assistant-host-bridge-e2e\"}")),
            "initialize request was not queued");
        Require(
            observation.WaitFor(Milestone::Initialized),
            "initialize did not complete within the timeout");

        Require(
            bridge.Post(Request(
                2,
                "22222222-2222-2222-2222-222222222222",
                "conversation.create",
                "{\"title\":\"Bridge E2E smoke\"}")),
            "conversation.create request was not queued");
        Require(
            observation.WaitFor(Milestone::ConversationCreated),
            "conversation.create did not complete within the timeout");

        const std::string conversationId =
            observation.ActiveConversationId();
        const std::string turnPayload =
            "{\"turnId\":\"33333333-3333-3333-3333-333333333333\","
            "\"conversationId\":\"" +
            cld::EscapeJson(conversationId) +
            "\",\"text\":\"bridge streaming smoke\"}";
        Require(
            bridge.Post(Request(
                3,
                "44444444-4444-4444-4444-444444444444",
                "sendTurn",
                turnPayload)),
            "sendTurn request was not queued");
        Require(
            observation.WaitFor(Milestone::TurnCompleted),
            "streaming turn did not complete within the timeout");

        Require(
            bridge.Post(Request(
                4,
                "55555555-5555-5555-5555-555555555555",
                "shutdown",
                "{}")),
            "shutdown request was not queued");
        Require(
            observation.WaitFor(Milestone::ShutdownAcknowledged),
            "shutdown was not acknowledged within the timeout");
        Require(
            observation.WaitFor(Milestone::Stopped),
            "ChatHost did not stop within the timeout");
        bridge.StopAndJoin();
        Require(
            observation.WaitFor(Milestone::ChildExitedCleanly),
            "ChatHost did not exit cleanly with exit code zero");
    }
    catch (...)
    {
        bridge.StopAndJoin();
        throw;
    }
}
}

int wmain(int argumentCount, wchar_t** arguments)
{
    try
    {
        Require(argumentCount == 2, "ChatHost path argument is required");
        Run(std::filesystem::path(arguments[1]));
        return 0;
    }
    catch (...)
    {
        std::cerr << "AssistantHostBridge E2E failed.\n";
        return 1;
    }
}
