// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace wsl::virtio_sound::audio_ipc {

inline constexpr std::uint32_t c_magic = 0x31534157U;
inline constexpr std::uint16_t c_version = 2U;
inline constexpr std::size_t c_headerBytes = 32U;
inline constexpr std::size_t c_maxPayloadBytes = 192000U;
inline constexpr std::size_t c_maxMessageBytes = c_headerBytes + c_maxPayloadBytes;
inline constexpr std::size_t c_maxEndpointCodeUnits = 1024U;
inline constexpr std::size_t c_startFixedPayloadBytes = 8U;
inline constexpr std::size_t c_maxStartPayloadBytes = c_startFixedPayloadBytes + (2U * c_maxEndpointCodeUnits);

enum class MessageType : std::uint16_t
{
    Hello = 1U,
    Ready = 2U,
    Start = 3U,
    Started = 4U,
    Stop = 5U,
    Stopped = 6U,
    Pcm = 7U,
    Fault = 8U,
    Shutdown = 9U,
    Closed = 10U,
    ServerIdentity = 11U,
};

enum class Error
{
    None,
    NullInput,
    TruncatedHeader,
    TruncatedMessage,
    TrailingBytes,
    InvalidMagic,
    UnsupportedVersion,
    UnknownType,
    InvalidReserved,
    ZeroSequence,
    InvalidSequence,
    InvalidEpoch,
    PayloadTooLarge,
    InvalidPayloadSize,
    InvalidPayloadValue,
    InvalidUtf16,
    InvalidArgument,
    AllocationFailure,
};

// This is a semantic in-memory representation. The wire format is always
// serialized field-by-field as little-endian bytes; this type is not packed.
struct Header
{
    std::uint32_t m_magic{c_magic};
    std::uint16_t m_version{c_version};
    MessageType m_type{MessageType::Hello};
    std::uint32_t m_payloadBytes{0U};
    std::uint32_t m_reserved{0U};
    std::uint64_t m_epoch{0U};
    std::uint64_t m_sequence{1U};
};

struct Message
{
    Header m_header{};
    std::vector<std::uint8_t> m_payload;
};

// Decode and validate only the first 32 bytes. `size` may exceed the header
// length so streaming callers can inspect a header before receiving payload.
// The function never allocates and leaves `out` unchanged on failure.
bool DecodeHeader(const std::uint8_t* data, std::size_t size, Header& out, Error& error) noexcept;

// Validate an already-owned message without allocating or copying its payload.
// The header's declared payloadBytes must equal payload.size().
bool ValidateMessage(const Message& message, Error& error) noexcept;

// Decode one exact wire message. The payload is validated before allocation;
// truncated and trailing bytes are rejected. `out` is unchanged on failure.
bool DecodeMessage(const std::uint8_t* data, std::size_t size, Message& out, Error& error);

// Encode one validated message to its exact wire representation. `out` is
// unchanged on failure.
bool EncodeMessage(const Message& message, std::vector<std::uint8_t>& out, Error& error);

// Build or parse Start's fixed-format payload. Endpoint IDs use UTF-16 code
// units encoded explicitly as little-endian bytes, independent of wchar_t.
bool BuildStartPayload(const std::u16string& endpointId, std::vector<std::uint8_t>& out, Error& error);

bool ParseStartPayload(const std::uint8_t* data, std::size_t size, std::u16string& endpointId, Error& error);

// The one-time ServerIdentity bootstrap payload is a single little-endian
// 64-bit target-process handle value. Handle-width and process-identity checks
// belong to the receiving Windows endpoint, not this portable codec.
bool BuildServerIdentityPayload(std::uint64_t targetProcessHandle, std::vector<std::uint8_t>& out, Error& error);

bool ParseServerIdentityPayload(const std::uint8_t* data, std::size_t size, std::uint64_t& targetProcessHandle, Error& error) noexcept;

} // namespace wsl::virtio_sound::audio_ipc
