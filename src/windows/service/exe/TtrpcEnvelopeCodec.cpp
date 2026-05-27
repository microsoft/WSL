// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"

#include "TtrpcEnvelopeCodec.h"

using namespace wsl::windows::service::wslc::detail;

void TtrpcEnvelopeCodec::WriteBigEndian32(uint8_t* dest, uint32_t value)
{
    dest[0] = static_cast<uint8_t>((value >> 24) & 0xFF);
    dest[1] = static_cast<uint8_t>((value >> 16) & 0xFF);
    dest[2] = static_cast<uint8_t>((value >> 8) & 0xFF);
    dest[3] = static_cast<uint8_t>(value & 0xFF);
}

uint32_t TtrpcEnvelopeCodec::ReadBigEndian32(const uint8_t* src)
{
    return (static_cast<uint32_t>(src[0]) << 24) |
           (static_cast<uint32_t>(src[1]) << 16) |
           (static_cast<uint32_t>(src[2]) << 8) |
           static_cast<uint32_t>(src[3]);
}

HRESULT TtrpcEnvelopeCodec::ReadVarint(const uint8_t*& ptr, const uint8_t* end, uint64_t& value)
{
    value = 0;
    int shift = 0;
    constexpr int c_maxVarintBytes = 10; // 64-bit varint is at most 10 bytes
    int bytesRead = 0;

    while (ptr < end)
    {
        RETURN_HR_IF_MSG(E_FAIL, bytesRead >= c_maxVarintBytes, "ttrpc: varint too large");

        const uint8_t byte = *ptr++;
        bytesRead++;

        value |= static_cast<uint64_t>(byte & 0x7F) << shift;

        if ((byte & 0x80) == 0)
        {
            return S_OK;
        }

        shift += 7;
    }

    return E_FAIL;
}

void TtrpcEnvelopeCodec::EncodeVarint(uint64_t value, std::vector<uint8_t>& buf)
{
    do
    {
        uint8_t byte = static_cast<uint8_t>(value & 0x7F);
        value >>= 7;
        if (value != 0)
        {
            byte |= 0x80;
        }
        buf.push_back(byte);
    } while (value != 0);
}

void TtrpcEnvelopeCodec::EncodeTag(uint32_t field, uint32_t wireType, std::vector<uint8_t>& buf)
{
    EncodeVarint((static_cast<uint64_t>(field) << 3) | wireType, buf);
}

void TtrpcEnvelopeCodec::EncodeStringField(uint32_t field, const std::string& value, std::vector<uint8_t>& buf)
{
    if (value.empty())
    {
        return;
    }

    EncodeTag(field, c_wireTypeLengthDelimited, buf);
    EncodeVarint(value.size(), buf);
    buf.insert(buf.end(), value.begin(), value.end());
}

void TtrpcEnvelopeCodec::EncodeBytesField(uint32_t field, const std::vector<uint8_t>& value, std::vector<uint8_t>& buf)
{
    if (value.empty())
    {
        return;
    }

    EncodeTag(field, c_wireTypeLengthDelimited, buf);
    EncodeVarint(value.size(), buf);
    buf.insert(buf.end(), value.begin(), value.end());
}

std::vector<uint8_t> TtrpcEnvelopeCodec::EncodeRequestEnvelope(
    const std::string& service,
    const std::string& method,
    const std::vector<uint8_t>& payload)
{
    std::vector<uint8_t> buf;
    EncodeStringField(1, service, buf);
    EncodeStringField(2, method, buf);
    EncodeBytesField(3, payload, buf);
    return buf;
}

HRESULT TtrpcEnvelopeCodec::DecodeResponseEnvelope(
    const std::vector<uint8_t>& responseData,
    DecodedResponse& decoded)
{
    decoded = {};

    const uint8_t* ptr = responseData.data();
    const uint8_t* end = ptr + responseData.size();

    while (ptr < end)
    {
        uint64_t tag = 0;
        RETURN_IF_FAILED(ReadVarint(ptr, end, tag));

        const uint32_t fieldNumber = static_cast<uint32_t>(tag >> 3);
        const uint32_t wireType = static_cast<uint32_t>(tag & 0x7);

        if (wireType == c_wireTypeVarint)
        {
            uint64_t ignored = 0;
            RETURN_IF_FAILED(ReadVarint(ptr, end, ignored));
            continue;
        }

        if (wireType != c_wireTypeLengthDelimited)
        {
            return E_FAIL;
        }

        uint64_t length = 0;
        RETURN_IF_FAILED(ReadVarint(ptr, end, length));

        RETURN_HR_IF_MSG(E_FAIL, length > static_cast<uint64_t>(end - ptr), "ttrpc: response truncated");

        if (fieldNumber == 1)
        {
            decoded.HasStatus = true;
            const uint8_t* statusEnd = ptr + length;

            while (ptr < statusEnd)
            {
                uint64_t innerTag = 0;
                RETURN_IF_FAILED(ReadVarint(ptr, statusEnd, innerTag));

                const uint32_t innerField = static_cast<uint32_t>(innerTag >> 3);
                const uint32_t innerWire = static_cast<uint32_t>(innerTag & 0x7);

                if (innerWire == c_wireTypeVarint)
                {
                    uint64_t value = 0;
                    RETURN_IF_FAILED(ReadVarint(ptr, statusEnd, value));

                    if (innerField == 1)
                    {
                        decoded.StatusCode = static_cast<int32_t>(value);
                    }
                }
                else if (innerWire == c_wireTypeLengthDelimited)
                {
                    uint64_t innerLength = 0;
                    RETURN_IF_FAILED(ReadVarint(ptr, statusEnd, innerLength));

                    RETURN_HR_IF_MSG(E_FAIL, innerLength > static_cast<uint64_t>(statusEnd - ptr), "ttrpc: status payload truncated");

                    if (innerField == 2)
                    {
                        decoded.StatusMessage.assign(reinterpret_cast<const char*>(ptr), static_cast<size_t>(innerLength));
                    }

                    ptr += innerLength;
                }
                else
                {
                    ptr = statusEnd;
                }
            }

            continue;
        }

        if (fieldNumber == 2)
        {
            RETURN_HR_IF_MSG(
                E_FAIL,
                length > c_maxMessageBytes,
                "ttrpc: response payload too large: %llu bytes",
                static_cast<unsigned long long>(length));

            decoded.Payload.assign(ptr, ptr + length);
            ptr += length;
            continue;
        }

        ptr += length;
    }

    return S_OK;
}
