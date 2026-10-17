// Copyright (C) Microsoft Corporation. All rights reserved.

#ifdef _WIN32
#include "precomp.h"
#endif

#include <algorithm>
#include <limits>
#include <new>
#include <utility>

#include "AudioIpcCodec.hpp"

namespace wsl::virtio_sound::audio_ipc {
namespace {

    constexpr std::uint32_t c_sampleRate = 48000U;
    constexpr std::uint16_t c_channels = 2U;
    constexpr std::uint16_t c_bitsPerSample = 16U;
    constexpr std::size_t c_faultPayloadBytes = 4U;

    std::uint16_t ReadU16Le(const std::uint8_t* data) noexcept
    {
        return static_cast<std::uint16_t>(data[0]) | static_cast<std::uint16_t>(static_cast<std::uint16_t>(data[1]) << 8U);
    }

    std::uint32_t ReadU32Le(const std::uint8_t* data) noexcept
    {
        return static_cast<std::uint32_t>(data[0]) | (static_cast<std::uint32_t>(data[1]) << 8U) |
               (static_cast<std::uint32_t>(data[2]) << 16U) | (static_cast<std::uint32_t>(data[3]) << 24U);
    }

    std::uint64_t ReadU64Le(const std::uint8_t* data) noexcept
    {
        std::uint64_t value = 0U;
        for (std::size_t index = 0U; index < 8U; ++index)
        {
            value |= static_cast<std::uint64_t>(data[index]) << (index * 8U);
        }
        return value;
    }

    void WriteU16Le(std::uint8_t* data, std::uint16_t value) noexcept
    {
        data[0] = static_cast<std::uint8_t>(value & 0xFFU);
        data[1] = static_cast<std::uint8_t>((value >> 8U) & 0xFFU);
    }

    void WriteU32Le(std::uint8_t* data, std::uint32_t value) noexcept
    {
        for (std::size_t index = 0U; index < 4U; ++index)
        {
            data[index] = static_cast<std::uint8_t>((value >> (index * 8U)) & 0xFFU);
        }
    }

    void WriteU64Le(std::uint8_t* data, std::uint64_t value) noexcept
    {
        for (std::size_t index = 0U; index < 8U; ++index)
        {
            data[index] = static_cast<std::uint8_t>((value >> (index * 8U)) & 0xFFU);
        }
    }

    bool IsKnownType(MessageType type) noexcept
    {
        switch (type)
        {
        case MessageType::Hello:
        case MessageType::Ready:
        case MessageType::Start:
        case MessageType::Started:
        case MessageType::Stop:
        case MessageType::Stopped:
        case MessageType::Pcm:
        case MessageType::Fault:
        case MessageType::Shutdown:
        case MessageType::Closed:
        case MessageType::ServerIdentity:
            return true;
        }
        return false;
    }

    bool ValidateHeaderFields(const Header& header, Error& error) noexcept
    {
        if (header.m_magic != c_magic)
        {
            error = Error::InvalidMagic;
            return false;
        }
        if (header.m_version != c_version)
        {
            error = Error::UnsupportedVersion;
            return false;
        }
        if (!IsKnownType(header.m_type))
        {
            error = Error::UnknownType;
            return false;
        }
        if (header.m_reserved != 0U)
        {
            error = Error::InvalidReserved;
            return false;
        }
        if (header.m_sequence == 0U)
        {
            error = Error::ZeroSequence;
            return false;
        }
        if (header.m_payloadBytes > c_maxPayloadBytes)
        {
            error = Error::PayloadTooLarge;
            return false;
        }

        switch (header.m_type)
        {
        case MessageType::Hello:
        case MessageType::Ready:
        case MessageType::Shutdown:
        case MessageType::Closed:
            if (header.m_epoch != 0U)
            {
                error = Error::InvalidEpoch;
                return false;
            }
            if (header.m_payloadBytes != 0U)
            {
                error = Error::InvalidPayloadSize;
                return false;
            }
            break;
        case MessageType::ServerIdentity:
            if (header.m_epoch != 0U)
            {
                error = Error::InvalidEpoch;
                return false;
            }
            if (header.m_sequence != 1U)
            {
                error = Error::InvalidSequence;
                return false;
            }
            if (header.m_payloadBytes != sizeof(std::uint64_t))
            {
                error = Error::InvalidPayloadSize;
                return false;
            }
            break;
        case MessageType::Start:
            if (header.m_epoch == 0U)
            {
                error = Error::InvalidEpoch;
                return false;
            }
            if (header.m_payloadBytes > c_maxStartPayloadBytes)
            {
                error = Error::PayloadTooLarge;
                return false;
            }
            if (header.m_payloadBytes < c_startFixedPayloadBytes + 2U || ((header.m_payloadBytes - c_startFixedPayloadBytes) % 2U) != 0U)
            {
                error = Error::InvalidPayloadSize;
                return false;
            }
            break;
        case MessageType::Started:
        case MessageType::Stop:
        case MessageType::Stopped:
            if (header.m_epoch == 0U)
            {
                error = Error::InvalidEpoch;
                return false;
            }
            if (header.m_payloadBytes != 0U)
            {
                error = Error::InvalidPayloadSize;
                return false;
            }
            break;
        case MessageType::Pcm:
            if (header.m_epoch == 0U)
            {
                error = Error::InvalidEpoch;
                return false;
            }
            if (header.m_payloadBytes == 0U || (header.m_payloadBytes % 4U) != 0U)
            {
                error = Error::InvalidPayloadSize;
                return false;
            }
            break;
        case MessageType::Fault:
            if (header.m_payloadBytes != c_faultPayloadBytes)
            {
                error = Error::InvalidPayloadSize;
                return false;
            }
            break;
        }

        error = Error::None;
        return true;
    }

    bool ValidateUtf16Le(const std::uint8_t* data, std::size_t size, Error& error) noexcept
    {
        if (data == nullptr)
        {
            error = Error::NullInput;
            return false;
        }
        if (size == 0U || (size % 2U) != 0U || (size / 2U) > c_maxEndpointCodeUnits)
        {
            error = Error::InvalidPayloadSize;
            return false;
        }

        const std::size_t codeUnitCount = size / 2U;
        for (std::size_t index = 0U; index < codeUnitCount; ++index)
        {
            const std::uint16_t codeUnit = ReadU16Le(data + (index * 2U));
            if (codeUnit == 0U)
            {
                error = Error::InvalidUtf16;
                return false;
            }
            if (codeUnit >= 0xD800U && codeUnit <= 0xDBFFU)
            {
                if ((index + 1U) >= codeUnitCount)
                {
                    error = Error::InvalidUtf16;
                    return false;
                }
                const std::uint16_t low = ReadU16Le(data + ((index + 1U) * 2U));
                if (low < 0xDC00U || low > 0xDFFFU)
                {
                    error = Error::InvalidUtf16;
                    return false;
                }
                ++index;
            }
            else if (codeUnit >= 0xDC00U && codeUnit <= 0xDFFFU)
            {
                error = Error::InvalidUtf16;
                return false;
            }
        }

        error = Error::None;
        return true;
    }

    bool ValidateStartPayload(const std::uint8_t* data, std::size_t size, Error& error) noexcept
    {
        if (data == nullptr)
        {
            error = Error::NullInput;
            return false;
        }
        if (size > c_maxStartPayloadBytes)
        {
            error = Error::PayloadTooLarge;
            return false;
        }
        if (size < c_startFixedPayloadBytes + 2U || ((size - c_startFixedPayloadBytes) % 2U) != 0U)
        {
            error = Error::InvalidPayloadSize;
            return false;
        }
        if (ReadU32Le(data) != c_sampleRate || ReadU16Le(data + 4U) != c_channels || ReadU16Le(data + 6U) != c_bitsPerSample)
        {
            error = Error::InvalidPayloadValue;
            return false;
        }
        return ValidateUtf16Le(data + c_startFixedPayloadBytes, size - c_startFixedPayloadBytes, error);
    }

    bool ValidatePayload(const Header& header, const std::uint8_t* payload, std::size_t size, Error& error) noexcept
    {
        switch (header.m_type)
        {
        case MessageType::Start:
            return ValidateStartPayload(payload, size, error);
        case MessageType::Fault:
            if (payload == nullptr)
            {
                error = Error::NullInput;
                return false;
            }
            if (ReadU32Le(payload) == 0U)
            {
                error = Error::InvalidPayloadValue;
                return false;
            }
            break;
        case MessageType::ServerIdentity:
        {
            std::uint64_t targetProcessHandle = 0U;
            return ParseServerIdentityPayload(payload, size, targetProcessHandle, error);
        }
        case MessageType::Hello:
        case MessageType::Ready:
        case MessageType::Started:
        case MessageType::Stop:
        case MessageType::Stopped:
        case MessageType::Pcm:
        case MessageType::Shutdown:
        case MessageType::Closed:
            break;
        }
        error = Error::None;
        return true;
    }

    void WriteHeader(const Header& header, std::uint8_t* data) noexcept
    {
        WriteU32Le(data, header.m_magic);
        WriteU16Le(data + 4U, header.m_version);
        WriteU16Le(data + 6U, static_cast<std::uint16_t>(header.m_type));
        WriteU32Le(data + 8U, header.m_payloadBytes);
        WriteU32Le(data + 12U, header.m_reserved);
        WriteU64Le(data + 16U, header.m_epoch);
        WriteU64Le(data + 24U, header.m_sequence);
    }

} // namespace

bool DecodeHeader(const std::uint8_t* data, std::size_t size, Header& out, Error& error) noexcept
{
    if (data == nullptr)
    {
        error = Error::NullInput;
        return false;
    }
    if (size < c_headerBytes)
    {
        error = Error::TruncatedHeader;
        return false;
    }

    Header decoded{};
    decoded.m_magic = ReadU32Le(data);
    decoded.m_version = ReadU16Le(data + 4U);
    decoded.m_type = static_cast<MessageType>(ReadU16Le(data + 6U));
    decoded.m_payloadBytes = ReadU32Le(data + 8U);
    decoded.m_reserved = ReadU32Le(data + 12U);
    decoded.m_epoch = ReadU64Le(data + 16U);
    decoded.m_sequence = ReadU64Le(data + 24U);
    if (!ValidateHeaderFields(decoded, error))
    {
        return false;
    }

    out = decoded;
    error = Error::None;
    return true;
}

bool ValidateMessage(const Message& message, Error& error) noexcept
{
    if (message.m_payload.size() > c_maxPayloadBytes)
    {
        error = Error::PayloadTooLarge;
        return false;
    }
    if (message.m_header.m_payloadBytes != message.m_payload.size())
    {
        error = Error::InvalidPayloadSize;
        return false;
    }
    if (!ValidateHeaderFields(message.m_header, error))
    {
        return false;
    }
    return ValidatePayload(message.m_header, message.m_payload.empty() ? nullptr : message.m_payload.data(), message.m_payload.size(), error);
}

bool DecodeMessage(const std::uint8_t* data, std::size_t size, Message& out, Error& error)
{
    Header header{};
    if (!DecodeHeader(data, size, header, error))
    {
        return false;
    }

    const std::size_t expectedSize = c_headerBytes + header.m_payloadBytes;
    if (size < expectedSize)
    {
        error = Error::TruncatedMessage;
        return false;
    }
    if (size > expectedSize)
    {
        error = Error::TrailingBytes;
        return false;
    }

    const std::uint8_t* payload = data + c_headerBytes;
    if (!ValidatePayload(header, header.m_payloadBytes == 0U ? nullptr : payload, header.m_payloadBytes, error))
    {
        return false;
    }

    Message decoded{};
    decoded.m_header = header;
    try
    {
        if (header.m_payloadBytes != 0U)
        {
            decoded.m_payload.assign(payload, payload + header.m_payloadBytes);
        }
    }
    catch (const std::bad_alloc&)
    {
        error = Error::AllocationFailure;
        return false;
    }

    out = std::move(decoded);
    error = Error::None;
    return true;
}

bool EncodeMessage(const Message& message, std::vector<std::uint8_t>& out, Error& error)
{
    if (!ValidateMessage(message, error))
    {
        return false;
    }

    const std::size_t totalSize = c_headerBytes + message.m_payload.size();
    std::vector<std::uint8_t> encoded;
    try
    {
        encoded.resize(totalSize);
    }
    catch (const std::bad_alloc&)
    {
        error = Error::AllocationFailure;
        return false;
    }

    WriteHeader(message.m_header, encoded.data());
    if (!message.m_payload.empty())
    {
        std::copy(message.m_payload.begin(), message.m_payload.end(), encoded.begin() + static_cast<std::ptrdiff_t>(c_headerBytes));
    }
    out.swap(encoded);
    error = Error::None;
    return true;
}

bool BuildStartPayload(const std::u16string& endpointId, std::vector<std::uint8_t>& out, Error& error)
{
    if (endpointId.empty())
    {
        error = Error::InvalidPayloadSize;
        return false;
    }
    if (endpointId.size() > c_maxEndpointCodeUnits)
    {
        error = Error::PayloadTooLarge;
        return false;
    }

    for (std::size_t index = 0U; index < endpointId.size(); ++index)
    {
        const std::uint16_t codeUnit = endpointId[index];
        if (codeUnit == 0U)
        {
            error = Error::InvalidUtf16;
            return false;
        }
        if (codeUnit >= 0xD800U && codeUnit <= 0xDBFFU)
        {
            if ((index + 1U) >= endpointId.size())
            {
                error = Error::InvalidUtf16;
                return false;
            }
            const std::uint16_t low = endpointId[index + 1U];
            if (low < 0xDC00U || low > 0xDFFFU)
            {
                error = Error::InvalidUtf16;
                return false;
            }
            ++index;
        }
        else if (codeUnit >= 0xDC00U && codeUnit <= 0xDFFFU)
        {
            error = Error::InvalidUtf16;
            return false;
        }
    }

    std::vector<std::uint8_t> payload;
    try
    {
        payload.resize(c_startFixedPayloadBytes + (endpointId.size() * 2U));
    }
    catch (const std::bad_alloc&)
    {
        error = Error::AllocationFailure;
        return false;
    }
    WriteU32Le(payload.data(), c_sampleRate);
    WriteU16Le(payload.data() + 4U, c_channels);
    WriteU16Le(payload.data() + 6U, c_bitsPerSample);
    for (std::size_t index = 0U; index < endpointId.size(); ++index)
    {
        WriteU16Le(payload.data() + c_startFixedPayloadBytes + (index * 2U), static_cast<std::uint16_t>(endpointId[index]));
    }

    out.swap(payload);
    error = Error::None;
    return true;
}

bool ParseStartPayload(const std::uint8_t* data, std::size_t size, std::u16string& endpointId, Error& error)
{
    if (!ValidateStartPayload(data, size, error))
    {
        return false;
    }

    const std::size_t codeUnitCount = (size - c_startFixedPayloadBytes) / 2U;
    std::u16string parsed;
    try
    {
        parsed.reserve(codeUnitCount);
        for (std::size_t index = 0U; index < codeUnitCount; ++index)
        {
            parsed.push_back(static_cast<char16_t>(ReadU16Le(data + c_startFixedPayloadBytes + (index * 2U))));
        }
    }
    catch (const std::bad_alloc&)
    {
        error = Error::AllocationFailure;
        return false;
    }

    endpointId.swap(parsed);
    error = Error::None;
    return true;
}

bool BuildServerIdentityPayload(std::uint64_t targetProcessHandle, std::vector<std::uint8_t>& out, Error& error)
{
    constexpr std::uint64_t c_invalidHandle = 0U;
    constexpr std::uint64_t c_pseudoProcessHandle = std::numeric_limits<std::uint64_t>::max();
    if (targetProcessHandle == c_invalidHandle || targetProcessHandle == c_pseudoProcessHandle ||
        targetProcessHandle == (c_pseudoProcessHandle - 1U))
    {
        error = Error::InvalidPayloadValue;
        return false;
    }

    std::vector<std::uint8_t> payload;
    try
    {
        payload.resize(sizeof(targetProcessHandle));
    }
    catch (const std::bad_alloc&)
    {
        error = Error::AllocationFailure;
        return false;
    }
    WriteU64Le(payload.data(), targetProcessHandle);
    out.swap(payload);
    error = Error::None;
    return true;
}

bool ParseServerIdentityPayload(const std::uint8_t* data, std::size_t size, std::uint64_t& targetProcessHandle, Error& error) noexcept
{
    if (data == nullptr)
    {
        error = Error::NullInput;
        return false;
    }
    if (size != sizeof(std::uint64_t))
    {
        error = Error::InvalidPayloadSize;
        return false;
    }

    constexpr std::uint64_t c_pseudoProcessHandle = std::numeric_limits<std::uint64_t>::max();
    const std::uint64_t parsedHandle = ReadU64Le(data);
    if (parsedHandle == 0U || parsedHandle == c_pseudoProcessHandle || parsedHandle == (c_pseudoProcessHandle - 1U))
    {
        error = Error::InvalidPayloadValue;
        return false;
    }

    targetProcessHandle = parsedHandle;
    error = Error::None;
    return true;
}

} // namespace wsl::virtio_sound::audio_ipc
