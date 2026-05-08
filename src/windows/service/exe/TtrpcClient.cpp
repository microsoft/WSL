// Copyright (C) Microsoft Corporation. All rights reserved.

/*++

Module Name:

    TtrpcClient.cpp

Abstract:

    Minimal ttrpc client for communicating with OpenVMM's vmservice.

    The ttrpc wire protocol is straightforward:
    1. A 10-byte header: 4 bytes big-endian length, 4 bytes big-endian stream ID,
       1 byte message type, 1 byte flags.
    2. A protobuf-encoded payload (Request for type 1, Response for type 2).

    The Request payload contains: service name, method name, inner payload
    (the actual protobuf request message), timeout, and metadata.

    The Response payload is a oneof: either a Status (error) at field 1,
    or a Payload (success bytes) at field 2.

    This implementation encodes protobuf manually for the small set of message
    types we need, avoiding a dependency on a full protobuf library.

    Wire format reference: openvmm/support/mesh/mesh_rpc/src/message.rs

--*/

#include "TtrpcClient.h"
#include <winsock2.h>
#include <afunix.h>
#include <format>
#include "stringshared.h"

using namespace wsl::windows::service::wslc;

TtrpcClient::TtrpcClient() = default;

TtrpcClient::~TtrpcClient()
{
    Disconnect();
}

HRESULT TtrpcClient::Connect(const std::wstring& socketPath, DWORD timeoutMs)
try
{
    std::lock_guard lock(m_lock);

    if (m_socket != INVALID_SOCKET)
    {
        return S_OK; // Already connected.
    }

    auto narrowPath = wsl::shared::string::WideToMultiByte(socketPath);

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    THROW_HR_IF_MSG(
        E_INVALIDARG,
        narrowPath.size() >= sizeof(addr.sun_path),
        "ttrpc socket path too long: %hs",
        narrowPath.c_str());
    memcpy(addr.sun_path, narrowPath.c_str(), narrowPath.size() + 1);

    // Retry with exponential backoff until the socket is available.
    // OpenVMM takes some time to start the ttrpc server after process launch.
    constexpr DWORD c_initialBackoffMs = 100;
    constexpr DWORD c_maxBackoffMs = 2000;
    DWORD elapsed = 0;
    DWORD backoff = c_initialBackoffMs;

    while (elapsed < timeoutMs)
    {
        SOCKET sock = ::socket(AF_UNIX, SOCK_STREAM, 0);
        THROW_LAST_ERROR_IF(sock == INVALID_SOCKET);

        if (::connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0)
        {
            m_socket = sock;
            m_nextStreamId = 1;

            WSL_LOG(
                "TtrpcClientConnected",
                TraceLoggingValue(narrowPath.c_str(), "SocketPath"),
                TraceLoggingValue(elapsed, "ElapsedMs"));

            return S_OK;
        }

        // Connection failed — the server may not be ready yet.
        closesocket(sock);

        DWORD sleepTime = std::min(backoff, timeoutMs - elapsed);
        Sleep(sleepTime);
        elapsed += sleepTime;
        backoff = std::min(backoff * 2, c_maxBackoffMs);
    }

    WSL_LOG(
        "TtrpcClientConnectTimeout",
        TraceLoggingValue(narrowPath.c_str(), "SocketPath"),
        TraceLoggingValue(timeoutMs, "TimeoutMs"));

    return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
}
CATCH_RETURN()

void TtrpcClient::Disconnect()
{
    std::lock_guard lock(m_lock);

    if (m_socket != INVALID_SOCKET)
    {
        closesocket(m_socket);
        m_socket = INVALID_SOCKET;
    }
}

bool TtrpcClient::IsConnected() const
{
    return m_socket != INVALID_SOCKET;
}

HRESULT TtrpcClient::AttachScsiDisk(
    uint32_t controller, uint32_t lun, const std::string& hostPath, bool readOnly)
try
{
    WSL_LOG(
        "TtrpcAttachScsiDisk",
        TraceLoggingValue(controller, "Controller"),
        TraceLoggingValue(lun, "Lun"),
        TraceLoggingValue(hostPath.c_str(), "HostPath"),
        TraceLoggingValue(readOnly, "ReadOnly"));

    auto scsiDisk = EncodeScsiDisk(controller, lun, hostPath, c_diskTypeVhdx, readOnly);
    auto request = EncodeModifyResourceRequest(c_modifyTypeAdd, scsiDisk);
    return SendRequest(c_serviceName, c_modifyResourceMethod, request);
}
CATCH_RETURN()

HRESULT TtrpcClient::DetachScsiDisk(uint32_t controller, uint32_t lun)
try
{
    WSL_LOG(
        "TtrpcDetachScsiDisk",
        TraceLoggingValue(controller, "Controller"),
        TraceLoggingValue(lun, "Lun"));

    // For REMOVE, we only need controller and lun — host_path and type are ignored.
    auto scsiDisk = EncodeScsiDisk(controller, lun, "", 0, false);
    auto request = EncodeModifyResourceRequest(c_modifyTypeRemove, scsiDisk);
    return SendRequest(c_serviceName, c_modifyResourceMethod, request);
}
CATCH_RETURN()

HRESULT TtrpcClient::CreateVm(const VmConfig& config)
try
{
    WSL_LOG(
        "TtrpcCreateVm",
        TraceLoggingValue(config.KernelPath.c_str(), "KernelPath"),
        TraceLoggingValue(config.MemoryMb, "MemoryMb"),
        TraceLoggingValue(config.ProcessorCount, "ProcessorCount"),
        TraceLoggingValue(static_cast<uint32_t>(config.ScsiDisks.size()), "DiskCount"),
        TraceLoggingValue(config.HvSocketPath.c_str(), "HvSocketPath"));

    auto payload = EncodeCreateVmRequest(config);
    return SendRequest(c_serviceName, c_createVmMethod, payload);
}
CATCH_RETURN()

HRESULT TtrpcClient::ResumeVm()
try
{
    WSL_LOG("TtrpcResumeVm");

    // ResumeVM takes google.protobuf.Empty — an empty payload.
    std::vector<uint8_t> emptyPayload;
    return SendRequest(c_serviceName, c_resumeVmMethod, emptyPayload);
}
CATCH_RETURN()

HRESULT TtrpcClient::SendRequest(
    const std::string& service, const std::string& method, const std::vector<uint8_t>& payload)
{
    std::lock_guard lock(m_lock);

    RETURN_HR_IF(HRESULT_FROM_WIN32(ERROR_NOT_CONNECTED), m_socket == INVALID_SOCKET);

    auto ttrpcPayload = EncodeTtrpcRequest(service, method, payload);

    // Build the 10-byte ttrpc message header.
    MessageHeader header{};
    WriteBigEndian32(header.Length, static_cast<uint32_t>(ttrpcPayload.size()));
    WriteBigEndian32(header.StreamId, m_nextStreamId);
    header.MessageType = c_messageTypeRequest;
    header.Flags = 0;

    uint32_t expectedStreamId = m_nextStreamId;
    m_nextStreamId += 2; // Client stream IDs must be odd.

    // Send the header and payload.
    RETURN_IF_FAILED(SendAll(&header, sizeof(header)));
    RETURN_IF_FAILED(SendAll(ttrpcPayload.data(), ttrpcPayload.size()));

    // Read the response header.
    MessageHeader responseHeader{};
    RETURN_IF_FAILED(RecvAll(&responseHeader, sizeof(responseHeader)));

    RETURN_HR_IF_MSG(
        E_FAIL,
        responseHeader.MessageType != c_messageTypeResponse,
        "ttrpc: expected response (type 2), got type %d",
        responseHeader.MessageType);

    uint32_t responseStreamId = ReadBigEndian32(responseHeader.StreamId);
    RETURN_HR_IF_MSG(
        E_FAIL,
        responseStreamId != expectedStreamId,
        "ttrpc: stream ID mismatch: expected %u, got %u",
        expectedStreamId,
        responseStreamId);

    uint32_t responseLength = ReadBigEndian32(responseHeader.Length);
    RETURN_HR_IF_MSG(
        E_FAIL,
        responseLength > 4 * 1024 * 1024, // 4MB max per ttrpc spec
        "ttrpc: response too large: %u bytes",
        responseLength);

    // Read the response payload.
    std::vector<uint8_t> responseData(responseLength);
    if (responseLength > 0)
    {
        RETURN_IF_FAILED(RecvAll(responseData.data(), responseLength));
    }

    return ParseTtrpcResponse(responseData);
}

HRESULT TtrpcClient::SendAll(const void* data, size_t length)
{
    const auto* ptr = static_cast<const char*>(data);
    size_t remaining = length;

    while (remaining > 0)
    {
        int sent = ::send(m_socket, ptr, static_cast<int>(remaining), 0);
        RETURN_LAST_ERROR_IF(sent == SOCKET_ERROR);
        RETURN_HR_IF(E_FAIL, sent == 0);
        ptr += sent;
        remaining -= sent;
    }

    return S_OK;
}

HRESULT TtrpcClient::RecvAll(void* data, size_t length)
{
    auto* ptr = static_cast<char*>(data);
    size_t remaining = length;

    while (remaining > 0)
    {
        int received = ::recv(m_socket, ptr, static_cast<int>(remaining), 0);
        RETURN_LAST_ERROR_IF(received == SOCKET_ERROR);
        RETURN_HR_IF_MSG(E_FAIL, received == 0, "ttrpc: connection closed unexpectedly");
        ptr += received;
        remaining -= received;
    }

    return S_OK;
}

// --- Protobuf Encoding Helpers ---
//
// Protobuf uses a tag-value encoding where each field is prefixed by a tag
// that encodes the field number and wire type:
//   tag = (field_number << 3) | wire_type
//
// Wire types:
//   0 = varint (int32, uint32, int64, uint64, bool, enum)
//   2 = length-delimited (string, bytes, embedded messages)
//
// Varints use 7 bits per byte with the MSB as a continuation flag.
//
// In proto3, fields with default values (0, false, empty string) are not
// serialized on the wire.

void TtrpcClient::EncodeVarint(uint64_t value, std::vector<uint8_t>& buf)
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

void TtrpcClient::EncodeTag(uint32_t field, uint32_t wireType, std::vector<uint8_t>& buf)
{
    EncodeVarint((static_cast<uint64_t>(field) << 3) | wireType, buf);
}

void TtrpcClient::EncodeVarintField(uint32_t field, uint64_t value, std::vector<uint8_t>& buf)
{
    if (value == 0)
    {
        return; // proto3: default values are not serialized.
    }
    EncodeTag(field, c_wireTypeVarint, buf);
    EncodeVarint(value, buf);
}

void TtrpcClient::EncodeStringField(uint32_t field, const std::string& value, std::vector<uint8_t>& buf)
{
    if (value.empty())
    {
        return; // proto3: default values are not serialized.
    }
    EncodeTag(field, c_wireTypeLengthDelimited, buf);
    EncodeVarint(value.size(), buf);
    buf.insert(buf.end(), value.begin(), value.end());
}

void TtrpcClient::EncodeBytesField(uint32_t field, const std::vector<uint8_t>& value, std::vector<uint8_t>& buf)
{
    if (value.empty())
    {
        return; // proto3: default values are not serialized.
    }
    EncodeTag(field, c_wireTypeLengthDelimited, buf);
    EncodeVarint(value.size(), buf);
    buf.insert(buf.end(), value.begin(), value.end());
}

void TtrpcClient::EncodeBoolField(uint32_t field, bool value, std::vector<uint8_t>& buf)
{
    if (!value)
    {
        return; // proto3: default values are not serialized.
    }
    EncodeTag(field, c_wireTypeVarint, buf);
    buf.push_back(1);
}

void TtrpcClient::WriteBigEndian32(uint8_t* dest, uint32_t value)
{
    dest[0] = static_cast<uint8_t>((value >> 24) & 0xFF);
    dest[1] = static_cast<uint8_t>((value >> 16) & 0xFF);
    dest[2] = static_cast<uint8_t>((value >> 8) & 0xFF);
    dest[3] = static_cast<uint8_t>(value & 0xFF);
}

uint32_t TtrpcClient::ReadBigEndian32(const uint8_t* src)
{
    return (static_cast<uint32_t>(src[0]) << 24) |
           (static_cast<uint32_t>(src[1]) << 16) |
           (static_cast<uint32_t>(src[2]) << 8) |
           static_cast<uint32_t>(src[3]);
}

// --- Protobuf Message Builders ---

// Encode a vmservice.SCSIDisk message:
//   field 1: controller (uint32)
//   field 2: lun (uint32)
//   field 3: host_path (string)
//   field 4: type (DiskType enum)
//   field 5: read_only (bool)
std::vector<uint8_t> TtrpcClient::EncodeScsiDisk(
    uint32_t controller, uint32_t lun, const std::string& hostPath, uint32_t diskType, bool readOnly)
{
    std::vector<uint8_t> buf;
    EncodeVarintField(1, controller, buf);
    EncodeVarintField(2, lun, buf);
    EncodeStringField(3, hostPath, buf);
    EncodeVarintField(4, diskType, buf);
    EncodeBoolField(5, readOnly, buf);
    return buf;
}

// Encode a vmservice.ModifyResourceRequest message:
//   field 1: type (ModifyType enum)
//   field 5: scsi_disk (SCSIDisk, oneof resource)
std::vector<uint8_t> TtrpcClient::EncodeModifyResourceRequest(
    uint32_t modifyType, const std::vector<uint8_t>& scsiDisk)
{
    std::vector<uint8_t> buf;
    EncodeVarintField(1, modifyType, buf);

    // scsi_disk is at field 5 in the oneof, encoded as a sub-message.
    if (!scsiDisk.empty())
    {
        EncodeTag(5, c_wireTypeLengthDelimited, buf);
        EncodeVarint(scsiDisk.size(), buf);
        buf.insert(buf.end(), scsiDisk.begin(), scsiDisk.end());
    }

    return buf;
}

// Encode a vmservice.CreateVMRequest message:
//   field 1: VMConfig (sub-message)
//   field 2: log_id (string) — omitted for now
//
// VMConfig:
//   field 1: MemoryConfig { field 1: memory_mb (uint64) }
//   field 2: ProcessorConfig { field 1: processor_count (uint32) }
//   field 3: DevicesConfig { field 1: repeated SCSIDisk }
//   field 5: DirectBoot { field 1: kernel_path, field 2: initrd_path, field 3: kernel_cmdline } [oneof BootConfig]
//   field 9: HVSocketConfig { field 1: path (string) }
std::vector<uint8_t> TtrpcClient::EncodeCreateVmRequest(const VmConfig& config)
{
    // Build MemoryConfig sub-message.
    std::vector<uint8_t> memoryConfig;
    EncodeVarintField(1, config.MemoryMb, memoryConfig);

    // Build ProcessorConfig sub-message.
    std::vector<uint8_t> processorConfig;
    EncodeVarintField(1, config.ProcessorCount, processorConfig);

    // Build DevicesConfig sub-message with SCSI disks and NIC.
    std::vector<uint8_t> devicesConfig;
    for (const auto& disk : config.ScsiDisks)
    {
        auto scsiDisk = EncodeScsiDisk(disk.Controller, disk.Lun, disk.HostPath, c_diskTypeVhdx, disk.ReadOnly);
        // DevicesConfig field 1 = repeated SCSIDisk (embedded message).
        EncodeTag(1, c_wireTypeLengthDelimited, devicesConfig);
        EncodeVarint(scsiDisk.size(), devicesConfig);
        devicesConfig.insert(devicesConfig.end(), scsiDisk.begin(), scsiDisk.end());
    }

    // NIC with consomme backend.
    if (config.Nic.has_value())
    {
        const auto& nic = config.Nic.value();

        // Build ConsommeBackend sub-message (all defaults — empty cidr uses
        // the server's default CIDR).
        std::vector<uint8_t> consommeBackend;
        // ConsommeBackend has no required fields; an empty message is valid.

        // Build NICConfig sub-message:
        //   field 1: nic_id (string)
        //   field 3: mac_address (string)
        //   field 8: consomme (ConsommeBackend, oneof backend)
        std::vector<uint8_t> nicConfig;
        EncodeStringField(1, nic.NicId, nicConfig);
        EncodeStringField(3, nic.MacAddress, nicConfig);
        // field 8 = ConsommeBackend (oneof backend). Even though the sub-message
        // is empty, we must encode it so the server sees the oneof variant.
        EncodeTag(8, c_wireTypeLengthDelimited, nicConfig);
        EncodeVarint(consommeBackend.size(), nicConfig);
        nicConfig.insert(nicConfig.end(), consommeBackend.begin(), consommeBackend.end());

        // DevicesConfig field 3 = repeated NICConfig.
        EncodeTag(3, c_wireTypeLengthDelimited, devicesConfig);
        EncodeVarint(nicConfig.size(), devicesConfig);
        devicesConfig.insert(devicesConfig.end(), nicConfig.begin(), nicConfig.end());
    }

    // Build DirectBoot sub-message.
    std::vector<uint8_t> directBoot;
    EncodeStringField(1, config.KernelPath, directBoot);
    EncodeStringField(2, config.InitrdPath, directBoot);
    EncodeStringField(3, config.KernelCmdLine, directBoot);

    // Build HVSocketConfig sub-message.
    std::vector<uint8_t> hvsocketConfig;
    EncodeStringField(1, config.HvSocketPath, hvsocketConfig);

    // Build VMConfig.
    std::vector<uint8_t> vmConfig;

    // field 1: MemoryConfig
    if (!memoryConfig.empty())
    {
        EncodeTag(1, c_wireTypeLengthDelimited, vmConfig);
        EncodeVarint(memoryConfig.size(), vmConfig);
        vmConfig.insert(vmConfig.end(), memoryConfig.begin(), memoryConfig.end());
    }

    // field 2: ProcessorConfig
    if (!processorConfig.empty())
    {
        EncodeTag(2, c_wireTypeLengthDelimited, vmConfig);
        EncodeVarint(processorConfig.size(), vmConfig);
        vmConfig.insert(vmConfig.end(), processorConfig.begin(), processorConfig.end());
    }

    // field 3: DevicesConfig
    if (!devicesConfig.empty())
    {
        EncodeTag(3, c_wireTypeLengthDelimited, vmConfig);
        EncodeVarint(devicesConfig.size(), vmConfig);
        vmConfig.insert(vmConfig.end(), devicesConfig.begin(), devicesConfig.end());
    }

    // field 5: DirectBoot (oneof BootConfig)
    if (!directBoot.empty())
    {
        EncodeTag(5, c_wireTypeLengthDelimited, vmConfig);
        EncodeVarint(directBoot.size(), vmConfig);
        vmConfig.insert(vmConfig.end(), directBoot.begin(), directBoot.end());
    }

    // field 9: HVSocketConfig
    if (!hvsocketConfig.empty())
    {
        EncodeTag(9, c_wireTypeLengthDelimited, vmConfig);
        EncodeVarint(hvsocketConfig.size(), vmConfig);
        vmConfig.insert(vmConfig.end(), hvsocketConfig.begin(), hvsocketConfig.end());
    }

    // Build CreateVMRequest: field 1 = VMConfig.
    std::vector<uint8_t> buf;
    EncodeTag(1, c_wireTypeLengthDelimited, buf);
    EncodeVarint(vmConfig.size(), buf);
    buf.insert(buf.end(), vmConfig.begin(), vmConfig.end());
    return buf;
}

// Encode the ttrpc Request envelope:
//   field 1: service (string)
//   field 2: method (string)
//   field 3: payload (bytes)
//   field 4: timeout_nano (uint64) — we leave at 0 (no timeout)
//   field 5: metadata — omitted
std::vector<uint8_t> TtrpcClient::EncodeTtrpcRequest(
    const std::string& service, const std::string& method, const std::vector<uint8_t>& payload)
{
    std::vector<uint8_t> buf;
    EncodeStringField(1, service, buf);
    EncodeStringField(2, method, buf);
    EncodeBytesField(3, payload, buf);
    // timeout_nano (field 4) = 0 → omitted (proto3 default).
    // metadata (field 5) = empty → omitted.
    return buf;
}

// Parse a ttrpc Response message.
//
// The response is a protobuf message with two possible fields:
//   field 1: Status (sub-message with code at field 1 and message at field 2) → error
//   field 2: Payload (bytes) → success (for ModifyResource this is google.protobuf.Empty)
//
// Returns S_OK if the response contains a payload (or is empty), or an error
// HRESULT if it contains a Status with a non-zero code.
HRESULT TtrpcClient::ParseTtrpcResponse(const std::vector<uint8_t>& responseData)
{
    const uint8_t* ptr = responseData.data();
    const uint8_t* end = ptr + responseData.size();

    bool hasStatus = false;
    int32_t statusCode = 0;
    std::string statusMessage;

    while (ptr < end)
    {
        // Decode field tag.
        uint64_t tag = 0;
        {
            uint64_t val = 0;
            int shift = 0;
            while (ptr < end)
            {
                uint8_t byte = *ptr++;
                val |= static_cast<uint64_t>(byte & 0x7F) << shift;
                shift += 7;
                if ((byte & 0x80) == 0)
                {
                    break;
                }
            }
            tag = val;
        }

        uint32_t fieldNumber = static_cast<uint32_t>(tag >> 3);
        uint32_t wireType = static_cast<uint32_t>(tag & 0x7);

        if (wireType == c_wireTypeVarint)
        {
            // Consume the varint value (we don't use it at this level).
            while (ptr < end && (*ptr & 0x80))
            {
                ptr++;
            }
            if (ptr < end)
            {
                ptr++;
            }
        }
        else if (wireType == c_wireTypeLengthDelimited)
        {
            // Decode length.
            uint64_t length = 0;
            {
                int shift = 0;
                while (ptr < end)
                {
                    uint8_t byte = *ptr++;
                    length |= static_cast<uint64_t>(byte & 0x7F) << shift;
                    shift += 7;
                    if ((byte & 0x80) == 0)
                    {
                        break;
                    }
                }
            }

            RETURN_HR_IF_MSG(E_FAIL, ptr + length > end, "ttrpc: response truncated");

            if (fieldNumber == 1)
            {
                // Field 1 = Status sub-message.
                // Parse Status { int32 code = 1; string message = 2; }
                hasStatus = true;
                const uint8_t* statusEnd = ptr + length;
                while (ptr < statusEnd)
                {
                    uint64_t innerTag = 0;
                    {
                        int shift = 0;
                        while (ptr < statusEnd)
                        {
                            uint8_t byte = *ptr++;
                            innerTag |= static_cast<uint64_t>(byte & 0x7F) << shift;
                            shift += 7;
                            if ((byte & 0x80) == 0)
                            {
                                break;
                            }
                        }
                    }

                    uint32_t innerField = static_cast<uint32_t>(innerTag >> 3);
                    uint32_t innerWire = static_cast<uint32_t>(innerTag & 0x7);

                    if (innerWire == c_wireTypeVarint)
                    {
                        uint64_t val = 0;
                        int shift = 0;
                        while (ptr < statusEnd)
                        {
                            uint8_t byte = *ptr++;
                            val |= static_cast<uint64_t>(byte & 0x7F) << shift;
                            shift += 7;
                            if ((byte & 0x80) == 0)
                            {
                                break;
                            }
                        }
                        if (innerField == 1)
                        {
                            statusCode = static_cast<int32_t>(val);
                        }
                    }
                    else if (innerWire == c_wireTypeLengthDelimited)
                    {
                        uint64_t innerLen = 0;
                        {
                            int shift = 0;
                            while (ptr < statusEnd)
                            {
                                uint8_t byte = *ptr++;
                                innerLen |= static_cast<uint64_t>(byte & 0x7F) << shift;
                                shift += 7;
                                if ((byte & 0x80) == 0)
                                {
                                    break;
                                }
                            }
                        }

                        if (innerField == 2 && ptr + innerLen <= statusEnd)
                        {
                            statusMessage.assign(reinterpret_cast<const char*>(ptr), static_cast<size_t>(innerLen));
                        }
                        ptr += innerLen;
                    }
                    else
                    {
                        // Unknown wire type in Status — skip to end.
                        ptr = statusEnd;
                    }
                }
            }
            else if (fieldNumber == 2)
            {
                // Field 2 = Payload (success). For ModifyResource this is Empty.
                // Skip over the payload bytes.
                ptr += length;
                return S_OK;
            }
            else
            {
                // Unknown field — skip.
                ptr += length;
            }
        }
        else
        {
            // Unknown wire type at the top level — we can't safely skip it
            // without knowing the size, so bail out.
            break;
        }
    }

    if (hasStatus && statusCode != 0)
    {
        WSL_LOG(
            "TtrpcRequestFailed",
            TraceLoggingValue(statusCode, "GrpcCode"),
            TraceLoggingValue(statusMessage.c_str(), "Message"));

        // Map common gRPC status codes to HRESULTs.
        switch (statusCode)
        {
        case 3: // INVALID_ARGUMENT
            return E_INVALIDARG;
        case 5: // NOT_FOUND
            return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
        case 12: // UNIMPLEMENTED
            return E_NOTIMPL;
        case 8: // RESOURCE_EXHAUSTED
            return HRESULT_FROM_WIN32(ERROR_NO_SYSTEM_RESOURCES);
        default:
            return E_FAIL;
        }
    }

    // Empty response with no status = success (e.g., google.protobuf.Empty).
    return S_OK;
}
