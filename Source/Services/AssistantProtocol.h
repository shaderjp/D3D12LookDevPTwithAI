#pragma once

#include "SimpleJson.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace lookdevpt::assistant
{
inline constexpr std::uint32_t ProtocolVersion = 1;
inline constexpr std::size_t MaximumPayloadBytes =
    4u * 1024u * 1024u;

enum class ProtocolErrorCode : std::uint8_t
{
    EmptyFrame,
    FrameTooLarge,
    InvalidUtf8,
    InvalidJson,
    InvalidEnvelope,
    UnsupportedVersion,
    InvalidSequence,
    NonMonotonicSequence,
    TruncatedFrame,
    DecoderFailed,
};

class ProtocolError final : public std::runtime_error
{
public:
    ProtocolError(
        ProtocolErrorCode code,
        char const* message);

    [[nodiscard]] ProtocolErrorCode Code() const noexcept;

private:
    ProtocolErrorCode m_code;
};

struct AssistantEnvelope
{
    std::uint32_t protocolVersion = 0;
    std::string requestId;
    std::uint64_t sequence = 0;
    cld::JsonValue root;
};

// Validates the UTF-8 JSON payload and the protocol envelope. Error messages
// deliberately identify only the invalid field and never include payload data.
[[nodiscard]] AssistantEnvelope ParseEnvelope(
    std::string_view payload);

// Encodes one validated envelope as a 32-bit little-endian byte count followed
// by its UTF-8 JSON bytes.
[[nodiscard]] std::vector<std::uint8_t> EncodeFrame(
    std::string_view payload);

class IncrementalFrameDecoder final
{
public:
    // Accepts any fragment size, including an empty fragment, and returns all
    // complete envelopes decoded from this fragment. A protocol violation puts
    // the decoder into a failed state because continuing the stream would be
    // ambiguous or unsafe.
    [[nodiscard]] std::vector<AssistantEnvelope> Push(
        std::span<std::uint8_t const> bytes);

    // Call when the pipe reaches EOF. A partial header or payload is a
    // protocol error, while a clean frame boundary succeeds.
    void Finish();

    void Reset() noexcept;

    [[nodiscard]] bool HasPartialFrame() const noexcept;
    [[nodiscard]] bool Failed() const noexcept;

private:
    [[noreturn]] void Fail(
        ProtocolErrorCode code,
        char const* message);
    void ResetFrame() noexcept;

    std::array<std::uint8_t, 4> m_header{};
    std::size_t m_headerBytes = 0;
    std::uint32_t m_payloadBytes = 0;
    std::string m_payload;
    std::optional<std::uint64_t> m_lastSequence;
    bool m_failed = false;
};
}
