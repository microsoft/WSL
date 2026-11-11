// Copyright (C) Microsoft Corporation. All rights reserved.

#ifdef _WIN32
#include "precomp.h"
#endif

#include "AudioIpcCodec.hpp"

#include <cstdint>
#include <iostream>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace {

using wsl::virtio_sound::audio_ipc::BuildServerIdentityPayload;
using wsl::virtio_sound::audio_ipc::BuildStartPayload;
using wsl::virtio_sound::audio_ipc::c_headerBytes;
using wsl::virtio_sound::audio_ipc::c_magic;
using wsl::virtio_sound::audio_ipc::c_maxEndpointCodeUnits;
using wsl::virtio_sound::audio_ipc::c_maxPayloadBytes;
using wsl::virtio_sound::audio_ipc::c_version;
using wsl::virtio_sound::audio_ipc::DecodeHeader;
using wsl::virtio_sound::audio_ipc::DecodeMessage;
using wsl::virtio_sound::audio_ipc::EncodeMessage;
using wsl::virtio_sound::audio_ipc::Error;
using wsl::virtio_sound::audio_ipc::Header;
using wsl::virtio_sound::audio_ipc::Message;
using wsl::virtio_sound::audio_ipc::MessageType;
using wsl::virtio_sound::audio_ipc::ParseServerIdentityPayload;
using wsl::virtio_sound::audio_ipc::ParseStartPayload;
using wsl::virtio_sound::audio_ipc::ValidateMessage;

int failures = 0;

bool Check(bool condition, const char* description)
{
    if (!condition)
    {
        std::cerr << "FAIL: " << description << '\n';
        ++failures;
    }
    return condition;
}

void PutU16(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint16_t value)
{
    bytes[offset] = static_cast<std::uint8_t>(value & 0xFFU);
    bytes[offset + 1U] = static_cast<std::uint8_t>((value >> 8U) & 0xFFU);
}

void PutU32(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint32_t value)
{
    for (std::size_t index = 0U; index < 4U; ++index)
    {
        bytes[offset + index] = static_cast<std::uint8_t>((value >> (index * 8U)) & 0xFFU);
    }
}

void PutU64(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint64_t value)
{
    for (std::size_t index = 0U; index < 8U; ++index)
    {
        bytes[offset + index] = static_cast<std::uint8_t>((value >> (index * 8U)) & 0xFFU);
    }
}

std::vector<std::uint8_t> MakeRawHeader(MessageType type, std::uint32_t payloadBytes, std::uint64_t epoch, std::uint64_t sequence)
{
    std::vector<std::uint8_t> bytes(c_headerBytes, 0U);
    PutU32(bytes, 0U, c_magic);
    PutU16(bytes, 4U, c_version);
    PutU16(bytes, 6U, static_cast<std::uint16_t>(type));
    PutU32(bytes, 8U, payloadBytes);
    PutU64(bytes, 16U, epoch);
    PutU64(bytes, 24U, sequence);
    return bytes;
}

Message MakeMessage(MessageType type, std::uint64_t epoch, std::uint64_t sequence, std::vector<std::uint8_t> payload = {})
{
    Message message{};
    message.m_header.m_type = type;
    message.m_header.m_epoch = epoch;
    message.m_header.m_sequence = sequence;
    message.m_header.m_payloadBytes = static_cast<std::uint32_t>(payload.size());
    message.m_payload = std::move(payload);
    return message;
}

bool ExpectHeaderError(std::vector<std::uint8_t> wire, Error expected)
{
    Header decoded{};
    Error error = Error::None;
    const bool result = DecodeHeader(wire.data(), wire.size(), decoded, error);
    return Check(!result && error == expected, "malformed header rejected");
}

bool ExpectMessageError(std::vector<std::uint8_t> wire, Error expected)
{
    Message decoded{};
    Error error = Error::None;
    const bool result = DecodeMessage(wire.data(), wire.size(), decoded, error);
    return Check(!result && error == expected, "malformed message rejected");
}

void TestControlRoundTripAndWireLayout()
{
    const Message hello = MakeMessage(MessageType::Hello, 0U, 1U);
    Error error = Error::None;
    std::vector<std::uint8_t> wire;
    Check(EncodeMessage(hello, wire, error), "Hello encodes");
    Check(wire.size() == c_headerBytes, "Hello has header-only size");
    Check(
        wire.size() >= 4U && wire[0] == 0x57U && wire[1] == 0x41U && wire[2] == 0x53U && wire[3] == 0x31U,
        "magic uses the specified little-endian bytes");
    Check(
        wire.size() >= 32U && wire[4] == 2U && wire[5] == 0U && wire[6] == 1U && wire[7] == 0U,
        "version and type use little-endian fields");

    Header header{};
    Check(DecodeHeader(wire.data(), wire.size(), header, error), "DecodeHeader accepts a complete header");
    Check(header.m_type == MessageType::Hello && header.m_sequence == 1U, "DecodeHeader exposes semantic fields");

    Message decoded{};
    Check(DecodeMessage(wire.data(), wire.size(), decoded, error), "Hello decodes");
    Check(
        decoded.m_header.m_type == hello.m_header.m_type && decoded.m_header.m_epoch == 0U && decoded.m_payload.empty(),
        "Hello round-trips");

    wire.push_back(0xAAU);
    Check(DecodeHeader(wire.data(), wire.size(), header, error), "DecodeHeader consumes only the first 32 bytes");
}

void TestLittleEndianWideCountersFixture()
{
    constexpr std::uint64_t c_epoch = 0x0102030405060708ULL;
    constexpr std::uint64_t c_sequence = 0x1112131415161718ULL;
    const Message started = MakeMessage(MessageType::Started, c_epoch, c_sequence);
    Error error = Error::None;
    std::vector<std::uint8_t> wire;
    Check(EncodeMessage(started, wire, error), "Started with wide epoch and sequence encodes");
    Check(
        wire.size() == c_headerBytes && wire[16U] == 0x08U && wire[17U] == 0x07U && wire[18U] == 0x06U && wire[19U] == 0x05U &&
            wire[20U] == 0x04U && wire[21U] == 0x03U && wire[22U] == 0x02U && wire[23U] == 0x01U,
        "epoch matches fixed little-endian wire fixture");
    Check(
        wire.size() == c_headerBytes && wire[24U] == 0x18U && wire[25U] == 0x17U && wire[26U] == 0x16U && wire[27U] == 0x15U &&
            wire[28U] == 0x14U && wire[29U] == 0x13U && wire[30U] == 0x12U && wire[31U] == 0x11U,
        "sequence matches fixed little-endian wire fixture");

    Message decoded{};
    Check(DecodeMessage(wire.data(), wire.size(), decoded, error), "wide-counter fixture decodes");
    Check(
        decoded.m_header.m_epoch == c_epoch && decoded.m_header.m_sequence == c_sequence,
        "wide counters preserve their full values");
}

void TestServerIdentityBootstrap()
{
    constexpr std::uint64_t c_targetProcessHandle = 0x0102030405060708ULL;
    Error error = Error::None;
    std::vector<std::uint8_t> payload;
    Check(BuildServerIdentityPayload(c_targetProcessHandle, payload, error), "valid server process handle builds");
    Check(
        payload.size() == 8U && payload[0U] == 0x08U && payload[1U] == 0x07U && payload[2U] == 0x06U && payload[3U] == 0x05U &&
            payload[4U] == 0x04U && payload[5U] == 0x03U && payload[6U] == 0x02U && payload[7U] == 0x01U,
        "server process handle uses exactly eight little-endian bytes");

    std::uint64_t parsedHandle = 0U;
    Check(
        ParseServerIdentityPayload(payload.data(), payload.size(), parsedHandle, error) && parsedHandle == c_targetProcessHandle,
        "server process handle payload parses without narrowing");

    const Message identity = MakeMessage(MessageType::ServerIdentity, 0U, 1U, payload);
    Check(ValidateMessage(identity, error), "ServerIdentity validates as the one-time epoch-zero sequence-one bootstrap");

    std::vector<std::uint8_t> wire;
    Check(EncodeMessage(identity, wire, error), "ServerIdentity bootstrap encodes");
    Check(
        wire.size() == c_headerBytes + 8U && wire[4U] == 2U && wire[5U] == 0U && wire[6U] == 11U && wire[7U] == 0U &&
            wire[16U] == 0U && wire[23U] == 0U && wire[24U] == 1U && wire[31U] == 0U && wire[32U] == 0x08U && wire[39U] == 0x01U,
        "wire fixture encodes v2/type 11/epoch zero/sequence one and handle LE");

    Message decoded{};
    Check(
        DecodeMessage(wire.data(), wire.size(), decoded, error) && decoded.m_header.m_type == MessageType::ServerIdentity &&
            decoded.m_header.m_epoch == 0U && decoded.m_header.m_sequence == 1U,
        "ServerIdentity wire message decodes with bootstrap header values");
    parsedHandle = 0U;
    Check(
        ParseServerIdentityPayload(decoded.m_payload.data(), decoded.m_payload.size(), parsedHandle, error) && parsedHandle == c_targetProcessHandle,
        "ServerIdentity message round-trips its handle value");

    std::vector<std::uint8_t> preservedPayload{0xA5U, 0x5AU};
    const std::vector<std::uint8_t> expectedPayload = preservedPayload;
    const std::uint64_t invalidHandles[] = {
        0U,
        std::numeric_limits<std::uint64_t>::max(),
        std::numeric_limits<std::uint64_t>::max() - 1U,
    };
    for (const std::uint64_t invalidHandle : invalidHandles)
    {
        Check(
            !BuildServerIdentityPayload(invalidHandle, preservedPayload, error) && error == Error::InvalidPayloadValue &&
                preservedPayload == expectedPayload,
            "builder rejects null and pseudo handle values without changing output");

        std::vector<std::uint8_t> invalidPayload(8U, 0U);
        PutU64(invalidPayload, 0U, invalidHandle);
        parsedHandle = 0x8877665544332211ULL;
        Check(
            !ParseServerIdentityPayload(invalidPayload.data(), invalidPayload.size(), parsedHandle, error) &&
                error == Error::InvalidPayloadValue && parsedHandle == 0x8877665544332211ULL,
            "parser rejects null and pseudo handle values without changing output");
    }

    parsedHandle = 0x1122334455667788ULL;
    Check(
        !ParseServerIdentityPayload(payload.data(), payload.size() - 1U, parsedHandle, error) &&
            error == Error::InvalidPayloadSize && parsedHandle == 0x1122334455667788ULL,
        "parser rejects non-eight-byte payload without changing output");

    const std::vector<std::uint8_t> invalidPayload(8U, 0U);
    const Message invalidIdentity = MakeMessage(MessageType::ServerIdentity, 0U, 1U, invalidPayload);
    std::vector<std::uint8_t> preservedWire{0xA5U, 0x5AU};
    const std::vector<std::uint8_t> expectedWire = preservedWire;
    Check(
        !EncodeMessage(invalidIdentity, preservedWire, error) && error == Error::InvalidPayloadValue && preservedWire == expectedWire,
        "encoder rejects an invalid identity without changing output");

    std::vector<std::uint8_t> invalidIdentityWire = MakeRawHeader(MessageType::ServerIdentity, 8U, 0U, 1U);
    invalidIdentityWire.insert(invalidIdentityWire.end(), invalidPayload.begin(), invalidPayload.end());
    Message preservedMessage = MakeMessage(MessageType::Hello, 0U, 9U);
    Check(
        !DecodeMessage(invalidIdentityWire.data(), invalidIdentityWire.size(), preservedMessage, error) &&
            error == Error::InvalidPayloadValue && preservedMessage.m_header.m_type == MessageType::Hello &&
            preservedMessage.m_header.m_sequence == 9U && preservedMessage.m_payload.empty(),
        "decoder rejects invalid identity without changing output");
}

void TestStartRoundTripAndUtf16Validation()
{
    const std::u16string endpoint = u"Default Endpoint \U0001F3A7";
    Error error = Error::None;
    std::vector<std::uint8_t> payload;
    Check(BuildStartPayload(endpoint, payload, error), "Start payload builds with a valid surrogate pair");

    Message start = MakeMessage(MessageType::Start, 9U, 2U, payload);
    Check(ValidateMessage(start, error), "valid Start passes allocation-free validation");
    std::vector<std::uint8_t> wire;
    Check(EncodeMessage(start, wire, error), "Start encodes");
    Message decoded{};
    Check(DecodeMessage(wire.data(), wire.size(), decoded, error), "Start decodes");

    std::u16string parsedEndpoint;
    Check(ParseStartPayload(decoded.m_payload.data(), decoded.m_payload.size(), parsedEndpoint, error), "Start endpoint parses");
    Check(parsedEndpoint == endpoint, "UTF-16 endpoint round-trips");

    const std::u16string embeddedNul{u'e', u'p', u'\0', u'i', u'd'};
    Check(!BuildStartPayload(embeddedNul, payload, error) && error == Error::InvalidUtf16, "embedded endpoint NUL is rejected");
    const std::u16string unpairedHigh{static_cast<char16_t>(0xD800U)};
    Check(!BuildStartPayload(unpairedHigh, payload, error) && error == Error::InvalidUtf16, "unpaired high surrogate is rejected");
    const std::u16string unpairedLow{static_cast<char16_t>(0xDC00U)};
    Check(!BuildStartPayload(unpairedLow, payload, error) && error == Error::InvalidUtf16, "unpaired low surrogate is rejected");
    Check(
        !BuildStartPayload(std::u16string{}, payload, error) && error == Error::InvalidPayloadSize, "empty endpoint is rejected");
    Check(
        !BuildStartPayload(std::u16string(1025U, u'x'), payload, error) && error == Error::PayloadTooLarge,
        "endpoint longer than 1024 code units is rejected");

    std::vector<std::uint8_t> badSurrogate = MakeRawHeader(MessageType::Start, 10U, 9U, 3U);
    badSurrogate.resize(42U, 0U);
    PutU32(badSurrogate, 32U, 48000U);
    PutU16(badSurrogate, 36U, 2U);
    PutU16(badSurrogate, 38U, 16U);
    PutU16(badSurrogate, 40U, 0xD800U);
    ExpectMessageError(std::move(badSurrogate), Error::InvalidUtf16);

    std::vector<std::uint8_t> badFormat = MakeRawHeader(MessageType::Start, 10U, 9U, 4U);
    badFormat.resize(42U, 0U);
    PutU32(badFormat, 32U, 44100U);
    PutU16(badFormat, 36U, 2U);
    PutU16(badFormat, 38U, 16U);
    PutU16(badFormat, 40U, static_cast<std::uint16_t>(u'x'));
    ExpectMessageError(std::move(badFormat), Error::InvalidPayloadValue);
}

void TestMaximumPayloadBoundaries()
{
    Error error = Error::None;
    std::vector<std::uint8_t> maxPcmPayload(c_maxPayloadBytes, static_cast<std::uint8_t>(0U));
    Message maxPcm = MakeMessage(MessageType::Pcm, 11U, 7U, std::move(maxPcmPayload));
    Check(ValidateMessage(maxPcm, error), "maximum-sized PCM payload validates");

    std::vector<std::uint8_t> wire;
    Check(EncodeMessage(maxPcm, wire, error), "maximum-sized PCM payload encodes");
    Check(wire.size() == c_headerBytes + c_maxPayloadBytes, "maximum-sized PCM message stays within the wire bound");
    Message decoded{};
    Check(DecodeMessage(wire.data(), wire.size(), decoded, error), "maximum-sized PCM payload decodes");
    Check(decoded.m_payload.size() == c_maxPayloadBytes, "maximum-sized PCM payload length is preserved");

    const std::uint32_t tooLargePcmBytes = static_cast<std::uint32_t>(c_maxPayloadBytes + 1U);
    std::vector<std::uint8_t> tooLargePcm = MakeRawHeader(MessageType::Pcm, tooLargePcmBytes, 11U, 8U);
    ExpectHeaderError(std::move(tooLargePcm), Error::PayloadTooLarge);

    Message tooLargeInMemory =
        MakeMessage(MessageType::Pcm, 11U, 9U, std::vector<std::uint8_t>(c_maxPayloadBytes + 1U, static_cast<std::uint8_t>(0U)));
    Check(
        !ValidateMessage(tooLargeInMemory, error) && error == Error::PayloadTooLarge,
        "one-over-limit PCM payload is rejected before traversal");

    const std::u16string maxEndpoint(c_maxEndpointCodeUnits, u'x');
    std::vector<std::uint8_t> startPayload;
    Check(BuildStartPayload(maxEndpoint, startPayload, error), "1024-code-unit endpoint is accepted");
    Check(startPayload.size() == 2056U, "maximum Start payload has its exact bounded length");
    Message maxStart = MakeMessage(MessageType::Start, 12U, 10U, std::move(startPayload));
    Check(EncodeMessage(maxStart, wire, error), "maximum Start payload encodes");
    Check(DecodeMessage(wire.data(), wire.size(), decoded, error), "maximum Start payload decodes");
    std::u16string parsedMaxEndpoint;
    Check(
        ParseStartPayload(decoded.m_payload.data(), decoded.m_payload.size(), parsedMaxEndpoint, error),
        "maximum endpoint parses");
    Check(parsedMaxEndpoint == maxEndpoint, "maximum endpoint round-trips without truncation");

    const std::u16string tooLongEndpoint(c_maxEndpointCodeUnits + 1U, u'x');
    Check(
        !BuildStartPayload(tooLongEndpoint, startPayload, error) && error == Error::PayloadTooLarge,
        "one-over-limit endpoint is rejected");
}

void TestPcmFaultAndControlMessages()
{
    Error error = Error::None;
    const std::vector<std::uint8_t> pcmBytes{0U, 1U, 2U, 3U, 4U, 5U, 6U, 7U};
    const Message pcm = MakeMessage(MessageType::Pcm, 44U, 3U, pcmBytes);
    std::vector<std::uint8_t> wire;
    Check(EncodeMessage(pcm, wire, error), "PCM encodes");
    Message decoded{};
    Check(DecodeMessage(wire.data(), wire.size(), decoded, error), "PCM decodes");
    Check(decoded.m_payload == pcmBytes, "PCM payload round-trips byte-for-byte");

    const Message fault = MakeMessage(MessageType::Fault, 44U, 4U, {17U, 0U, 0U, 0U});
    Check(EncodeMessage(fault, wire, error), "Fault encodes with a nonzero code");
    Check(DecodeMessage(wire.data(), wire.size(), decoded, error), "Fault decodes");
    Check(decoded.m_payload == fault.m_payload, "Fault code round-trips");

    const Message stopped = MakeMessage(MessageType::Stopped, 44U, 5U);
    Check(EncodeMessage(stopped, wire, error), "Stopped encodes without a payload");
    Check(DecodeMessage(wire.data(), wire.size(), decoded, error), "Stopped decodes without a payload");

    const Message shutdown = MakeMessage(MessageType::Shutdown, 0U, 6U);
    Check(EncodeMessage(shutdown, wire, error), "Shutdown encodes at epoch zero");
}

void TestMalformedHeadersAndLengths()
{
    std::vector<std::uint8_t> wire = MakeRawHeader(MessageType::Hello, 0U, 0U, 1U);
    wire[0] = 0U;
    ExpectHeaderError(wire, Error::InvalidMagic);

    wire = MakeRawHeader(MessageType::Hello, 0U, 0U, 1U);
    PutU16(wire, 4U, 1U);
    ExpectHeaderError(wire, Error::UnsupportedVersion);

    wire = MakeRawHeader(MessageType::Hello, 0U, 0U, 1U);
    PutU16(wire, 6U, 99U);
    ExpectHeaderError(wire, Error::UnknownType);

    wire = MakeRawHeader(MessageType::Hello, 0U, 0U, 1U);
    PutU32(wire, 12U, 1U);
    ExpectHeaderError(wire, Error::InvalidReserved);

    wire = MakeRawHeader(MessageType::Hello, 0U, 0U, 0U);
    ExpectHeaderError(wire, Error::ZeroSequence);

    wire = MakeRawHeader(MessageType::Hello, 0U, 1U, 1U);
    ExpectHeaderError(wire, Error::InvalidEpoch);

    wire = MakeRawHeader(MessageType::Pcm, 4U, 0U, 1U);
    ExpectHeaderError(wire, Error::InvalidEpoch);

    wire = MakeRawHeader(MessageType::Pcm, static_cast<std::uint32_t>(c_maxPayloadBytes + 1U), 1U, 1U);
    ExpectHeaderError(wire, Error::PayloadTooLarge);

    wire = MakeRawHeader(MessageType::Pcm, 5U, 1U, 1U);
    ExpectHeaderError(wire, Error::InvalidPayloadSize);

    wire = MakeRawHeader(MessageType::Pcm, 0U, 1U, 1U);
    ExpectHeaderError(wire, Error::InvalidPayloadSize);

    wire = MakeRawHeader(MessageType::Start, 9U, 1U, 1U);
    ExpectHeaderError(wire, Error::InvalidPayloadSize);

    wire = MakeRawHeader(MessageType::Start, 11U, 1U, 1U);
    ExpectHeaderError(wire, Error::InvalidPayloadSize);

    wire = MakeRawHeader(MessageType::Start, 2058U, 1U, 1U);
    ExpectHeaderError(wire, Error::PayloadTooLarge);

    wire = MakeRawHeader(MessageType::Fault, 8U, 0U, 1U);
    ExpectHeaderError(wire, Error::InvalidPayloadSize);

    wire = MakeRawHeader(MessageType::ServerIdentity, 7U, 0U, 1U);
    ExpectHeaderError(wire, Error::InvalidPayloadSize);

    wire = MakeRawHeader(MessageType::ServerIdentity, 9U, 0U, 1U);
    ExpectHeaderError(wire, Error::InvalidPayloadSize);

    wire = MakeRawHeader(MessageType::ServerIdentity, 8U, 1U, 1U);
    ExpectHeaderError(wire, Error::InvalidEpoch);

    wire = MakeRawHeader(MessageType::ServerIdentity, 8U, 0U, 2U);
    ExpectHeaderError(wire, Error::InvalidSequence);

    wire = MakeRawHeader(MessageType::ServerIdentity, 8U, 0U, 0U);
    ExpectHeaderError(wire, Error::ZeroSequence);

    wire = MakeRawHeader(MessageType::Ready, 4U, 0U, 1U);
    ExpectHeaderError(wire, Error::InvalidPayloadSize);

    wire = MakeRawHeader(MessageType::Hello, 0U, 0U, 1U);
    wire.resize(31U);
    ExpectHeaderError(wire, Error::TruncatedHeader);
}

void TestMalformedPayloadsAndExactMessageSizes()
{
    std::vector<std::uint8_t> wire = MakeRawHeader(MessageType::Fault, 4U, 0U, 1U);
    wire.resize(36U, 0U);
    ExpectMessageError(wire, Error::InvalidPayloadValue);

    wire = MakeRawHeader(MessageType::Pcm, 4U, 1U, 1U);
    wire.resize(35U, 0U);
    ExpectMessageError(wire, Error::TruncatedMessage);

    wire = MakeRawHeader(MessageType::Hello, 0U, 0U, 1U);
    wire.push_back(0U);
    ExpectMessageError(wire, Error::TrailingBytes);

    wire = MakeRawHeader(MessageType::Pcm, 4U, 1U, 1U);
    wire.resize(36U, 0U);
    wire[35U] = 1U;
    wire.push_back(0U);
    ExpectMessageError(wire, Error::TrailingBytes);

    Header decoded{};
    Message decodedMessage{};
    Error error = Error::None;
    Check(!DecodeHeader(nullptr, 0U, decoded, error) && error == Error::NullInput, "null header input is rejected");
    Check(!DecodeMessage(nullptr, 0U, decodedMessage, error) && error == Error::NullInput, "null message input is rejected");

    Message mismatch = MakeMessage(MessageType::Pcm, 1U, 1U, {0U, 0U, 0U, 0U});
    mismatch.m_header.m_payloadBytes = 8U;
    Check(!ValidateMessage(mismatch, error) && error == Error::InvalidPayloadSize, "in-memory declared-size mismatch is rejected");

    Message zeroSequence = MakeMessage(MessageType::Hello, 0U, 0U);
    std::vector<std::uint8_t> encoded;
    Check(!EncodeMessage(zeroSequence, encoded, error) && error == Error::ZeroSequence, "encoder rejects zero sequence numbers");

    const std::vector<std::uint8_t> preservedWire{0xA5U, 0x5AU};
    encoded = preservedWire;
    Check(
        !EncodeMessage(zeroSequence, encoded, error) && encoded == preservedWire,
        "failed encode leaves the caller output unchanged");

    Header preservedHeader{};
    preservedHeader.m_type = MessageType::Stopped;
    preservedHeader.m_epoch = 99U;
    preservedHeader.m_sequence = 88U;
    const Header expectedHeader = preservedHeader;
    std::vector<std::uint8_t> invalidHeader = MakeRawHeader(MessageType::Hello, 0U, 1U, 1U);
    Check(
        !DecodeHeader(invalidHeader.data(), invalidHeader.size(), preservedHeader, error) && error == Error::InvalidEpoch &&
            preservedHeader.m_type == expectedHeader.m_type && preservedHeader.m_epoch == expectedHeader.m_epoch &&
            preservedHeader.m_sequence == expectedHeader.m_sequence,
        "failed header decode leaves the caller output unchanged");

    Message preservedMessage = MakeMessage(MessageType::Stopped, 99U, 88U, {0x11U, 0x22U});
    decodedMessage = preservedMessage;
    const std::vector<std::uint8_t> invalidMessage = MakeRawHeader(MessageType::Hello, 0U, 1U, 1U);
    Check(
        !DecodeMessage(invalidMessage.data(), invalidMessage.size(), decodedMessage, error) && error == Error::InvalidEpoch &&
            decodedMessage.m_header.m_type == preservedMessage.m_header.m_type &&
            decodedMessage.m_header.m_epoch == preservedMessage.m_header.m_epoch &&
            decodedMessage.m_header.m_sequence == preservedMessage.m_header.m_sequence &&
            decodedMessage.m_payload == preservedMessage.m_payload,
        "failed message decode leaves the caller output unchanged");

    std::vector<std::uint8_t> invalidStartPayload{0x80U, 0xBBU, 0x00U, 0x00U, 0x02U, 0x00U, 0x10U, 0x00U, 0x00U, 0xD8U};
    std::u16string preservedEndpoint = u"keep this endpoint";
    const std::u16string expectedEndpoint = preservedEndpoint;
    Check(
        !ParseStartPayload(invalidStartPayload.data(), invalidStartPayload.size(), preservedEndpoint, error) &&
            error == Error::InvalidUtf16 && preservedEndpoint == expectedEndpoint,
        "failed Start parse leaves the caller output unchanged");
}

} // namespace

int main()
{
    TestControlRoundTripAndWireLayout();
    TestLittleEndianWideCountersFixture();
    TestServerIdentityBootstrap();
    TestStartRoundTripAndUtf16Validation();
    TestMaximumPayloadBoundaries();
    TestPcmFaultAndControlMessages();
    TestMalformedHeadersAndLengths();
    TestMalformedPayloadsAndExactMessageSizes();

    if (failures != 0)
    {
        std::cerr << failures << " codec test(s) failed\n";
        return 1;
    }
    std::cout << "Audio IPC codec tests passed\n";
    return 0;
}
