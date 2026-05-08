// Copyright (C) Microsoft Corporation. All rights reserved.

/*++

Module Name:

    TtrpcClient.h

Abstract:

    Minimal ttrpc client for communicating with OpenVMM's vmservice.

    Implements the ttrpc wire protocol and protobuf encoding to call
    vmservice RPCs: CreateVM, ResumeVM, ModifyResource (SCSI disk
    hot-add/remove). The ttrpc protocol uses a 10-byte header (big-endian
    length, stream ID, type, flags) followed by a protobuf-encoded
    Request/Response payload.

    See: openvmm/support/mesh/mesh_rpc/src/message.rs for the wire format.

--*/

#pragma once

#include <string>
#include <vector>
#include <optional>
#include <cstdint>
#include <mutex>

namespace wsl::windows::service::wslc {

class TtrpcClient
{
public:
    TtrpcClient();
    ~TtrpcClient();

    NON_COPYABLE(TtrpcClient);
    NON_MOVABLE(TtrpcClient);

    // Connect to the ttrpc Unix domain socket at the given path.
    // Retries with backoff until the connection succeeds or timeoutMs expires.
    HRESULT Connect(const std::wstring& socketPath, DWORD timeoutMs = 30000);

    // Disconnect from the ttrpc server.
    void Disconnect();

    // Returns true if the client is connected.
    bool IsConnected() const;

    // SCSI disk hot-add: ModifyResource(ADD, SCSIDisk { controller, lun, hostPath, VHDX, readOnly }).
    HRESULT AttachScsiDisk(uint32_t controller, uint32_t lun,
                           const std::string& hostPath, bool readOnly);

    // SCSI disk hot-remove: ModifyResource(REMOVE, SCSIDisk { controller, lun }).
    HRESULT DetachScsiDisk(uint32_t controller, uint32_t lun);

    // VM configuration for CreateVm.
    struct VmConfig
    {
        std::string KernelPath;
        std::string InitrdPath;
        std::string KernelCmdLine;
        uint64_t MemoryMb{};
        uint32_t ProcessorCount{};
        std::string HvSocketPath;

        struct ScsiDisk
        {
            uint32_t Controller;
            uint32_t Lun;
            std::string HostPath;
            bool ReadOnly;
        };
        std::vector<ScsiDisk> ScsiDisks;

        // NIC with consomme backend (self-contained NAT + DHCP).
        struct ConsommeNic
        {
            std::string NicId;      // GUID string
            std::string MacAddress;  // "12-34-56-78-9A-BC"
        };
        std::optional<ConsommeNic> Nic;
    };

    // CreateVM: configure and create the VM (left in paused state).
    HRESULT CreateVm(const VmConfig& config);

    // ResumeVM: start a paused VM.
    HRESULT ResumeVm();

private:
    // ttrpc message types (from the ttrpc spec).
    static constexpr uint8_t c_messageTypeRequest = 1;
    static constexpr uint8_t c_messageTypeResponse = 2;

    // ttrpc service and method names (from vmservice.proto).
    static constexpr char c_serviceName[] = "vmservice.VM";
    static constexpr char c_createVmMethod[] = "CreateVM";
    static constexpr char c_resumeVmMethod[] = "ResumeVM";
    static constexpr char c_modifyResourceMethod[] = "ModifyResource";

    // vmservice.proto ModifyType enum values.
    static constexpr uint32_t c_modifyTypeAdd = 0;
    static constexpr uint32_t c_modifyTypeRemove = 1;

    // vmservice.proto DiskType enum values.
    static constexpr uint32_t c_diskTypeVhdx = 1;

    // Protobuf wire types.
    static constexpr uint32_t c_wireTypeVarint = 0;
    static constexpr uint32_t c_wireTypeLengthDelimited = 2;

#pragma pack(push, 1)
    struct MessageHeader
    {
        uint8_t Length[4];   // big-endian uint32
        uint8_t StreamId[4]; // big-endian uint32
        uint8_t MessageType;
        uint8_t Flags;
    };
#pragma pack(pop)

    static_assert(sizeof(MessageHeader) == 10, "ttrpc MessageHeader must be 10 bytes");

    // Send a ttrpc request and wait for the response.
    // Returns S_OK on success, or an HRESULT error if the server returned a
    // status error or there was a communication failure.
    HRESULT SendRequest(const std::string& service, const std::string& method,
                        const std::vector<uint8_t>& payload);

    // Low-level socket I/O.
    HRESULT SendAll(const void* data, size_t length);
    HRESULT RecvAll(void* data, size_t length);

    // Protobuf encoding helpers.
    static void EncodeVarint(uint64_t value, std::vector<uint8_t>& buf);
    static void EncodeTag(uint32_t field, uint32_t wireType, std::vector<uint8_t>& buf);
    static void EncodeVarintField(uint32_t field, uint64_t value, std::vector<uint8_t>& buf);
    static void EncodeStringField(uint32_t field, const std::string& value, std::vector<uint8_t>& buf);
    static void EncodeBytesField(uint32_t field, const std::vector<uint8_t>& value, std::vector<uint8_t>& buf);
    static void EncodeBoolField(uint32_t field, bool value, std::vector<uint8_t>& buf);

    // Big-endian helpers for the ttrpc header.
    static void WriteBigEndian32(uint8_t* dest, uint32_t value);
    static uint32_t ReadBigEndian32(const uint8_t* src);

    // Build protobuf-encoded SCSIDisk message.
    static std::vector<uint8_t> EncodeScsiDisk(uint32_t controller, uint32_t lun,
                                                const std::string& hostPath,
                                                uint32_t diskType, bool readOnly);

    // Build protobuf-encoded ModifyResourceRequest message.
    static std::vector<uint8_t> EncodeModifyResourceRequest(uint32_t modifyType,
                                                             const std::vector<uint8_t>& scsiDisk);

    // Build protobuf-encoded CreateVMRequest message.
    static std::vector<uint8_t> EncodeCreateVmRequest(const VmConfig& config);

    // Build protobuf-encoded ttrpc Request message.
    static std::vector<uint8_t> EncodeTtrpcRequest(const std::string& service,
                                                    const std::string& method,
                                                    const std::vector<uint8_t>& payload);

    // Parse a ttrpc Response message. Returns S_OK if the response indicates
    // success, or an error HRESULT with a log message on failure.
    static HRESULT ParseTtrpcResponse(const std::vector<uint8_t>& responseData);

    std::recursive_mutex m_lock;
    SOCKET m_socket = INVALID_SOCKET;
    uint32_t m_nextStreamId = 1; // ttrpc client stream IDs must be odd
};

} // namespace wsl::windows::service::wslc
