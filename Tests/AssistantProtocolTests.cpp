#include "Services/AssistantProtocol.h"

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

using namespace lookdevpt::assistant;

namespace
{
void Require(bool condition, char const* message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

template<typename Callback>
void RequireError(
    Callback&& callback,
    ProtocolErrorCode expected,
    char const* message)
{
    try
    {
        callback();
    }
    catch (ProtocolError const& error)
    {
        Require(error.Code() == expected, message);
        return;
    }
    throw std::runtime_error(message);
}

std::string Envelope(
    std::uint64_t sequence,
    std::string requestId = "request-1")
{
    return
        "{\"protocolVersion\":1,\"requestId\":\"" +
        requestId + "\",\"sequence\":" +
        std::to_string(sequence) +
        ",\"type\":\"textDelta\",\"payload\":{}}";
}

std::vector<std::uint8_t> RawFrame(std::string const& payload)
{
    const auto size = static_cast<std::uint32_t>(payload.size());
    std::vector<std::uint8_t> frame = {
        static_cast<std::uint8_t>(size & 0xffu),
        static_cast<std::uint8_t>((size >> 8u) & 0xffu),
        static_cast<std::uint8_t>((size >> 16u) & 0xffu),
        static_cast<std::uint8_t>((size >> 24u) & 0xffu),
    };
    frame.insert(frame.end(), payload.begin(), payload.end());
    return frame;
}

std::vector<std::uint8_t> LengthHeader(std::uint32_t size)
{
    return {
        static_cast<std::uint8_t>(size & 0xffu),
        static_cast<std::uint8_t>((size >> 8u) & 0xffu),
        static_cast<std::uint8_t>((size >> 16u) & 0xffu),
        static_cast<std::uint8_t>((size >> 24u) & 0xffu),
    };
}
}

int main()
{
    const std::string firstJson = Envelope(1);
    const std::vector<std::uint8_t> firstFrame =
        EncodeFrame(firstJson);
    Require(firstFrame.size() == firstJson.size() + 4u,
        "encoded frame size is incorrect");
    Require(firstFrame[0] ==
            static_cast<std::uint8_t>(firstJson.size() & 0xffu) &&
            firstFrame[1] ==
            static_cast<std::uint8_t>(
                (firstJson.size() >> 8u) & 0xffu) &&
            firstFrame[2] == 0 && firstFrame[3] == 0,
        "frame length is not little endian");

    IncrementalFrameDecoder byteDecoder;
    std::vector<AssistantEnvelope> byteDecoded;
    for (const std::uint8_t byte : firstFrame)
    {
        const auto result = byteDecoder.Push(
            std::span<std::uint8_t const>(&byte, 1));
        byteDecoded.insert(
            byteDecoded.end(), result.begin(), result.end());
    }
    Require(byteDecoded.size() == 1,
        "byte-at-a-time frame was not decoded once");
    Require(byteDecoded[0].protocolVersion == ProtocolVersion &&
            byteDecoded[0].requestId == "request-1" &&
            byteDecoded[0].sequence == 1,
        "decoded envelope fields are incorrect");
    Require(!byteDecoder.HasPartialFrame(),
        "complete frame left partial decoder state");

    const auto secondFrame = EncodeFrame(Envelope(2, "request-2"));
    std::vector<std::uint8_t> combined = firstFrame;
    combined.insert(
        combined.end(), secondFrame.begin(), secondFrame.end());
    IncrementalFrameDecoder multipleDecoder;
    const auto multiple = multipleDecoder.Push(combined);
    Require(multiple.size() == 2 &&
            multiple[0].sequence == 1 &&
            multiple[1].sequence == 2,
        "multiple frames were not decoded in order");

    IncrementalFrameDecoder splitDecoder;
    Require(splitDecoder.Push(
                std::span<std::uint8_t const>(
                    firstFrame.data(), 2)).empty(),
        "partial header produced a frame");
    Require(splitDecoder.HasPartialFrame(),
        "partial header was not retained");
    const auto split = splitDecoder.Push(
        std::span<std::uint8_t const>(
            firstFrame.data() + 2,
            firstFrame.size() - 2));
    Require(split.size() == 1 && split[0].sequence == 1,
        "split header/body was not reassembled");
    splitDecoder.Finish();

    const std::string utf8Envelope =
        "{\"protocolVersion\":1,\"requestId\":\"utf8-1\","
        "\"sequence\":1,\"payload\":\""
        "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e\"}";
    Require(ParseEnvelope(utf8Envelope).requestId == "utf8-1",
        "valid UTF-8 payload was rejected");

    RequireError(
        [] { (void)EncodeFrame({}); },
        ProtocolErrorCode::EmptyFrame,
        "empty encoded payload was accepted");
    RequireError(
        []
        {
            IncrementalFrameDecoder decoder;
            const auto header = LengthHeader(0);
            (void)decoder.Push(header);
        },
        ProtocolErrorCode::EmptyFrame,
        "zero-length frame was accepted");
    RequireError(
        []
        {
            IncrementalFrameDecoder decoder;
            const auto header = LengthHeader(
                static_cast<std::uint32_t>(
                    MaximumPayloadBytes + 1u));
            (void)decoder.Push(header);
        },
        ProtocolErrorCode::FrameTooLarge,
        "oversized frame was accepted");

    std::string invalidUtf8 = Envelope(1);
    const std::string marker = "\"payload\":{}";
    const auto markerPosition = invalidUtf8.find(marker);
    invalidUtf8.replace(
        markerPosition,
        marker.size(),
        std::string("\"payload\":\"") +
            static_cast<char>(0xc0) +
            static_cast<char>(0xaf) + "\"");
    RequireError(
        [&]
        {
            IncrementalFrameDecoder decoder;
            const auto frame = RawFrame(invalidUtf8);
            (void)decoder.Push(frame);
        },
        ProtocolErrorCode::InvalidUtf8,
        "invalid UTF-8 was accepted");

    RequireError(
        []
        {
            IncrementalFrameDecoder decoder;
            const auto frame = RawFrame(
                "{\"protocolVersion\":1,");
            (void)decoder.Push(frame);
        },
        ProtocolErrorCode::InvalidJson,
        "invalid JSON was accepted");
    RequireError(
        []
        {
            std::string payload = Envelope(1);
            payload.insert(payload.find("request-1") + 3, 1, '\n');
            IncrementalFrameDecoder decoder;
            const auto frame = RawFrame(payload);
            (void)decoder.Push(frame);
        },
        ProtocolErrorCode::InvalidJson,
        "raw JSON string control character was accepted");
    RequireError(
        []
        {
            const auto frame = RawFrame(
                "{\"protocolVersion\":1,\v"
                "\"requestId\":\"request-1\",\"sequence\":1}");
            IncrementalFrameDecoder decoder;
            (void)decoder.Push(frame);
        },
        ProtocolErrorCode::InvalidJson,
        "invalid JSON whitespace was accepted");
    RequireError(
        []
        {
            (void)EncodeFrame(
                "{\"protocolVersion\":01,"
                "\"requestId\":\"request-1\",\"sequence\":1}");
        },
        ProtocolErrorCode::InvalidJson,
        "JSON number with a leading zero was accepted");
    RequireError(
        []
        {
            (void)EncodeFrame(
                "{\"protocolVersion\":1,\"sequence\":1}");
        },
        ProtocolErrorCode::InvalidEnvelope,
        "missing requestId was accepted");
    RequireError(
        []
        {
            (void)EncodeFrame(
                "{\"protocolVersion\":1,"
                "\"requestId\":\"not allowed\",\"sequence\":1}");
        },
        ProtocolErrorCode::InvalidEnvelope,
        "non-token requestId was accepted");
    RequireError(
        []
        {
            (void)EncodeFrame(
                "{\"protocolVersion\":2,"
                "\"requestId\":\"request-1\",\"sequence\":1}");
        },
        ProtocolErrorCode::UnsupportedVersion,
        "unsupported protocol version was accepted");
    RequireError(
        []
        {
            (void)ParseEnvelope(
                "{\"protocolVersion\":1,"
                "\"requestId\":\"request-0\",\"sequence\":0}");
        },
        ProtocolErrorCode::InvalidSequence,
        "zero sequence was accepted");
    RequireError(
        []
        {
            (void)EncodeFrame(
                "{\"protocolVersion\":1,"
                "\"requestId\":\"request-1\",\"sequence\":-1}");
        },
        ProtocolErrorCode::InvalidSequence,
        "negative sequence was accepted");
    RequireError(
        []
        {
            (void)EncodeFrame(
                "{\"protocolVersion\":1,"
                "\"requestId\":\"request-1\",\"sequence\":1.5}");
        },
        ProtocolErrorCode::InvalidSequence,
        "fractional sequence was accepted");

    IncrementalFrameDecoder sequenceDecoder;
    (void)sequenceDecoder.Push(EncodeFrame(Envelope(3)));
    RequireError(
        [&]
        {
            (void)sequenceDecoder.Push(EncodeFrame(Envelope(3)));
        },
        ProtocolErrorCode::NonMonotonicSequence,
        "duplicate sequence was accepted");
    Require(sequenceDecoder.Failed(),
        "decoder did not enter failed state");
    RequireError(
        [&]
        {
            (void)sequenceDecoder.Push(EncodeFrame(Envelope(4)));
        },
        ProtocolErrorCode::DecoderFailed,
        "failed decoder accepted more bytes");
    sequenceDecoder.Reset();
    const auto afterReset = sequenceDecoder.Push(
        EncodeFrame(Envelope(1)));
    Require(afterReset.size() == 1 && !sequenceDecoder.Failed(),
        "decoder did not recover after reset");

    IncrementalFrameDecoder truncatedDecoder;
    (void)truncatedDecoder.Push(
        std::span<std::uint8_t const>(
            firstFrame.data(), firstFrame.size() - 1));
    RequireError(
        [&] { truncatedDecoder.Finish(); },
        ProtocolErrorCode::TruncatedFrame,
        "truncated frame was accepted at end of stream");

    return 0;
}
