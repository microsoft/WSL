// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace wsl::windows::service::wslc::detail {

#pragma pack(push, 1)
struct TtrpcMessageHeader
{
    uint8_t Length[4];   // big-endian uint32
    uint8_t StreamId[4]; // big-endian uint32
    uint8_t MessageType;
    uint8_t Flags;
};
#pragma pack(pop)

static_assert(sizeof(TtrpcMessageHeader) == 10, "ttrpc MessageHeader must be 10 bytes");

class TtrpcEnvelopeCodec
{
public:
    static constexpr uint8_t c_messageTypeRequest = 1;
    static constexpr uint8_t c_messageTypeResponse = 2;
    static constexpr uint32_t c_maxMessageBytes = 4 * 1024 * 1024;

    struct DecodedResponse
    {
        bool HasStatus = false;
        int32_t StatusCode = 0;
        std::string StatusMessage;
        std::vector<uint8_t> Payload;
    };

    static void WriteBigEndian32(uint8_t* dest, uint32_t value);
    static uint32_t ReadBigEndian32(const uint8_t* src);

    static std::vector<uint8_t> EncodeRequestEnvelope(const std::string& service,
                                                      const std::string& method,
                                                      const std::vector<uint8_t>& payload);

    static HRESULT DecodeResponseEnvelope(const std::vector<uint8_t>& responseData,
                                          DecodedResponse& decoded);

private:
    static constexpr uint32_t c_wireTypeVarint = 0;
    static constexpr uint32_t c_wireTypeLengthDelimited = 2;

    static HRESULT ReadVarint(const uint8_t*& ptr, const uint8_t* end, uint64_t& value);
    static void EncodeVarint(uint64_t value, std::vector<uint8_t>& buf);
    static void EncodeTag(uint32_t field, uint32_t wireType, std::vector<uint8_t>& buf);
    static void EncodeStringField(uint32_t field, const std::string& value, std::vector<uint8_t>& buf);
    static void EncodeBytesField(uint32_t field, const std::vector<uint8_t>& value, std::vector<uint8_t>& buf);
};

} // namespace wsl::windows::service::wslc::detail
