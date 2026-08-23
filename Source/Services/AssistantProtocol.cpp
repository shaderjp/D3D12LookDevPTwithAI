#include "AssistantProtocol.h"

#include <algorithm>
#include <cmath>

namespace lookdevpt::assistant
{
namespace
{
constexpr std::size_t MaximumRequestIdBytes = 256;
constexpr std::uint64_t MaximumExactJsonInteger =
    (1ull << 53) - 1ull;

bool IsContinuationByte(std::uint8_t value) noexcept
{
    return (value & 0xc0u) == 0x80u;
}

bool IsValidUtf8(std::string_view text) noexcept
{
    const auto* bytes = reinterpret_cast<std::uint8_t const*>(
        text.data());
    std::size_t index = 0;
    while (index < text.size())
    {
        const std::uint8_t first = bytes[index++];
        if (first <= 0x7fu)
        {
            continue;
        }

        if (first >= 0xc2u && first <= 0xdfu)
        {
            if (index >= text.size() ||
                !IsContinuationByte(bytes[index]))
            {
                return false;
            }
            ++index;
            continue;
        }

        if (first >= 0xe0u && first <= 0xefu)
        {
            if (index + 1 >= text.size())
            {
                return false;
            }
            const std::uint8_t second = bytes[index];
            const std::uint8_t third = bytes[index + 1];
            if (!IsContinuationByte(third) ||
                (first == 0xe0u &&
                 (second < 0xa0u || second > 0xbfu)) ||
                (first == 0xedu &&
                 (second < 0x80u || second > 0x9fu)) ||
                (first != 0xe0u && first != 0xedu &&
                 !IsContinuationByte(second)))
            {
                return false;
            }
            index += 2;
            continue;
        }

        if (first >= 0xf0u && first <= 0xf4u)
        {
            if (index + 2 >= text.size())
            {
                return false;
            }
            const std::uint8_t second = bytes[index];
            const std::uint8_t third = bytes[index + 1];
            const std::uint8_t fourth = bytes[index + 2];
            if (!IsContinuationByte(third) ||
                !IsContinuationByte(fourth) ||
                (first == 0xf0u &&
                 (second < 0x90u || second > 0xbfu)) ||
                (first == 0xf4u &&
                 (second < 0x80u || second > 0x8fu)) ||
                (first != 0xf0u && first != 0xf4u &&
                 !IsContinuationByte(second)))
            {
                return false;
            }
            index += 3;
            continue;
        }

        return false;
    }
    return true;
}

bool ContainsInvalidRawControl(std::string_view text) noexcept
{
    bool inString = false;
    bool escaped = false;
    for (const unsigned char byte : text)
    {
        if (!inString)
        {
            if (byte == '"')
            {
                inString = true;
            }
            else if (byte < 0x20u &&
                     byte != '\t' && byte != '\n' && byte != '\r')
            {
                return true;
            }
            continue;
        }
        if (escaped)
        {
            escaped = false;
            continue;
        }
        if (byte == '\\')
        {
            escaped = true;
        }
        else if (byte == '"')
        {
            inString = false;
        }
        else if (byte < 0x20u)
        {
            return true;
        }
    }
    return false;
}

bool ContainsLeadingZeroNumber(std::string_view text) noexcept
{
    bool inString = false;
    bool escaped = false;
    std::size_t index = 0;
    while (index < text.size())
    {
        const unsigned char byte =
            static_cast<unsigned char>(text[index]);
        if (inString)
        {
            if (escaped)
            {
                escaped = false;
            }
            else if (byte == '\\')
            {
                escaped = true;
            }
            else if (byte == '"')
            {
                inString = false;
            }
            ++index;
            continue;
        }
        if (byte == '"')
        {
            inString = true;
            ++index;
            continue;
        }
        if (byte != '-' && (byte < '0' || byte > '9'))
        {
            ++index;
            continue;
        }

        std::size_t number = index;
        if (text[number] == '-')
        {
            ++number;
        }
        if (number + 1 < text.size() && text[number] == '0' &&
            text[number + 1] >= '0' && text[number + 1] <= '9')
        {
            return true;
        }

        // The existing parser performs the complete syntax check. Skipping
        // the candidate token here prevents a zero within 10 or 100 from
        // being mistaken for the start of another number.
        index = number;
        while (index < text.size() &&
               text[index] >= '0' && text[index] <= '9')
        {
            ++index;
        }
        if (index < text.size() && text[index] == '.')
        {
            ++index;
            while (index < text.size() &&
                   text[index] >= '0' && text[index] <= '9')
            {
                ++index;
            }
        }
        if (index < text.size() &&
            (text[index] == 'e' || text[index] == 'E'))
        {
            ++index;
            if (index < text.size() &&
                (text[index] == '+' || text[index] == '-'))
            {
                ++index;
            }
            while (index < text.size() &&
                   text[index] >= '0' && text[index] <= '9')
            {
                ++index;
            }
        }
    }
    return false;
}

std::uint64_t ReadInteger(
    cld::JsonValue const& root,
    char const* name,
    double minimum,
    ProtocolErrorCode code)
{
    const cld::JsonValue* member = cld::FindMember(root, name);
    if (!member || member->type != cld::JsonValue::Type::Number ||
        !std::isfinite(member->number) ||
        member->number < minimum ||
        member->number >
            static_cast<double>(MaximumExactJsonInteger) ||
        std::floor(member->number) != member->number)
    {
        throw ProtocolError(code,
            "Assistant envelope integer field is invalid.");
    }
    return static_cast<std::uint64_t>(member->number);
}

std::string ReadRequestId(cld::JsonValue const& root)
{
    const cld::JsonValue* member =
        cld::FindMember(root, "requestId");
    if (!member || member->type != cld::JsonValue::Type::String ||
        member->string.empty() ||
        member->string.size() > MaximumRequestIdBytes ||
        !std::all_of(
            member->string.begin(), member->string.end(),
            [](unsigned char value)
            {
                return (value >= 'a' && value <= 'z') ||
                    (value >= 'A' && value <= 'Z') ||
                    (value >= '0' && value <= '9') ||
                    value == '-' || value == '_' ||
                    value == '.' || value == ':';
            }))
    {
        throw ProtocolError(
            ProtocolErrorCode::InvalidEnvelope,
            "Assistant envelope requestId is invalid.");
    }
    return member->string;
}
}

ProtocolError::ProtocolError(
    ProtocolErrorCode code,
    char const* message)
    : std::runtime_error(message),
      m_code(code)
{
}

ProtocolErrorCode ProtocolError::Code() const noexcept
{
    return m_code;
}

AssistantEnvelope ParseEnvelope(std::string_view payload)
{
    if (payload.empty())
    {
        throw ProtocolError(
            ProtocolErrorCode::EmptyFrame,
            "Assistant frame payload is empty.");
    }
    if (payload.size() > MaximumPayloadBytes)
    {
        throw ProtocolError(
            ProtocolErrorCode::FrameTooLarge,
            "Assistant frame payload exceeds the size limit.");
    }
    if (!IsValidUtf8(payload))
    {
        throw ProtocolError(
            ProtocolErrorCode::InvalidUtf8,
            "Assistant frame payload is not valid UTF-8.");
    }
    if (ContainsInvalidRawControl(payload) ||
        ContainsLeadingZeroNumber(payload))
    {
        throw ProtocolError(
            ProtocolErrorCode::InvalidJson,
            "Assistant frame payload is not valid JSON.");
    }

    cld::JsonValue root;
    try
    {
        const std::string text(payload);
        root = cld::JsonParser(text).Parse();
    }
    catch (std::bad_alloc const&)
    {
        throw;
    }
    catch (std::exception const&)
    {
        throw ProtocolError(
            ProtocolErrorCode::InvalidJson,
            "Assistant frame payload is not valid JSON.");
    }

    if (root.type != cld::JsonValue::Type::Object)
    {
        throw ProtocolError(
            ProtocolErrorCode::InvalidEnvelope,
            "Assistant frame JSON must be an object.");
    }

    const std::uint64_t version = ReadInteger(
        root,
        "protocolVersion",
        1.0,
        ProtocolErrorCode::InvalidEnvelope);
    if (version != ProtocolVersion)
    {
        throw ProtocolError(
            ProtocolErrorCode::UnsupportedVersion,
            "Assistant protocol version is unsupported.");
    }

    AssistantEnvelope envelope;
    envelope.protocolVersion = static_cast<std::uint32_t>(version);
    envelope.requestId = ReadRequestId(root);
    envelope.sequence = ReadInteger(
        root,
        "sequence",
        1.0,
        ProtocolErrorCode::InvalidSequence);
    envelope.root = std::move(root);
    return envelope;
}

std::vector<std::uint8_t> EncodeFrame(std::string_view payload)
{
    (void)ParseEnvelope(payload);

    const auto payloadBytes =
        static_cast<std::uint32_t>(payload.size());
    std::vector<std::uint8_t> frame;
    frame.reserve(4u + payload.size());
    frame.push_back(static_cast<std::uint8_t>(payloadBytes & 0xffu));
    frame.push_back(static_cast<std::uint8_t>(
        (payloadBytes >> 8u) & 0xffu));
    frame.push_back(static_cast<std::uint8_t>(
        (payloadBytes >> 16u) & 0xffu));
    frame.push_back(static_cast<std::uint8_t>(
        (payloadBytes >> 24u) & 0xffu));
    frame.insert(frame.end(), payload.begin(), payload.end());
    return frame;
}

std::vector<AssistantEnvelope> IncrementalFrameDecoder::Push(
    std::span<std::uint8_t const> bytes)
{
    if (m_failed)
    {
        throw ProtocolError(
            ProtocolErrorCode::DecoderFailed,
            "Assistant frame decoder must be reset after failure.");
    }

    std::vector<AssistantEnvelope> decoded;
    std::size_t offset = 0;
    try
    {
        while (offset < bytes.size())
        {
            if (m_headerBytes < m_header.size())
            {
                const std::size_t count = (std::min)(
                    m_header.size() - m_headerBytes,
                    bytes.size() - offset);
                std::copy_n(
                    bytes.begin() +
                        static_cast<std::ptrdiff_t>(offset),
                    count,
                    m_header.begin() +
                        static_cast<std::ptrdiff_t>(m_headerBytes));
                m_headerBytes += count;
                offset += count;
                if (m_headerBytes < m_header.size())
                {
                    continue;
                }

                m_payloadBytes =
                    static_cast<std::uint32_t>(m_header[0]) |
                    (static_cast<std::uint32_t>(m_header[1]) << 8u) |
                    (static_cast<std::uint32_t>(m_header[2]) << 16u) |
                    (static_cast<std::uint32_t>(m_header[3]) << 24u);
                if (m_payloadBytes == 0)
                {
                    Fail(
                        ProtocolErrorCode::EmptyFrame,
                        "Assistant frame payload is empty.");
                }
                if (m_payloadBytes > MaximumPayloadBytes)
                {
                    Fail(
                        ProtocolErrorCode::FrameTooLarge,
                        "Assistant frame payload exceeds the size limit.");
                }
                m_payload.clear();
                m_payload.reserve(m_payloadBytes);
            }

            const std::size_t count = (std::min)(
                static_cast<std::size_t>(m_payloadBytes) -
                    m_payload.size(),
                bytes.size() - offset);
            m_payload.append(
                reinterpret_cast<char const*>(
                    bytes.data() + offset),
                count);
            offset += count;
            if (m_payload.size() < m_payloadBytes)
            {
                continue;
            }

            AssistantEnvelope envelope = ParseEnvelope(m_payload);
            if (m_lastSequence &&
                envelope.sequence <= *m_lastSequence)
            {
                Fail(
                    ProtocolErrorCode::NonMonotonicSequence,
                    "Assistant envelope sequence is not increasing.");
            }
            m_lastSequence = envelope.sequence;
            decoded.emplace_back(std::move(envelope));
            ResetFrame();
        }
    }
    catch (...)
    {
        m_failed = true;
        throw;
    }
    return decoded;
}

void IncrementalFrameDecoder::Finish()
{
    if (m_failed)
    {
        throw ProtocolError(
            ProtocolErrorCode::DecoderFailed,
            "Assistant frame decoder must be reset after failure.");
    }
    if (HasPartialFrame())
    {
        Fail(
            ProtocolErrorCode::TruncatedFrame,
            "Assistant stream ended with a partial frame.");
    }
}

void IncrementalFrameDecoder::Reset() noexcept
{
    ResetFrame();
    m_lastSequence.reset();
    m_failed = false;
}

bool IncrementalFrameDecoder::HasPartialFrame() const noexcept
{
    return m_headerBytes != 0 || !m_payload.empty();
}

bool IncrementalFrameDecoder::Failed() const noexcept
{
    return m_failed;
}

[[noreturn]] void IncrementalFrameDecoder::Fail(
    ProtocolErrorCode code,
    char const* message)
{
    m_failed = true;
    throw ProtocolError(code, message);
}

void IncrementalFrameDecoder::ResetFrame() noexcept
{
    m_header.fill(0);
    m_headerBytes = 0;
    m_payloadBytes = 0;
    m_payload.clear();
}
}
