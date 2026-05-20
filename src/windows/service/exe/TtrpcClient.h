// Copyright (C) Microsoft Corporation. All rights reserved.

/*++

Module Name:

    TtrpcClient.h

Abstract:

    Minimal ttrpc client for communicating with OpenVMM's vmservice.

    Implements the ttrpc wire protocol and uses protobuf payloads generated
    from VMService.proto for vmservice RPCs.

    The ttrpc protocol uses a 10-byte header (big-endian length, stream ID,
    type, flags) followed by a protobuf-encoded Request/Response payload.

    See: openvmm/support/mesh/mesh_rpc/src/message.rs for the wire format.

--*/

#pragma once

#include <string>
#include <vector>
#include <optional>
#include <cstdint>
#include <mutex>

namespace google::protobuf {
class Message;
}

namespace wsl::windows::service::wslc {

class TtrpcClient
{
public:
    TtrpcClient();
    ~TtrpcClient();

    NON_COPYABLE(TtrpcClient);
    NON_MOVABLE(TtrpcClient);

    // Default timeout for Connect() retries and socket I/O operations.
    static constexpr DWORD c_defaultTimeoutMs = 30000;

    // Connect to the ttrpc Unix domain socket at the given path.
    // Retries with backoff until the connection succeeds or timeoutMs expires.
    HRESULT Connect(const std::wstring& socketPath, DWORD timeoutMs = c_defaultTimeoutMs);

    // Disconnect from the ttrpc server.
    void Disconnect();

    // Returns true if the client is connected.
    bool IsConnected() const;

    // Generic ttrpc call using protobuf request/response messages.
    // If response is null, any successful payload is ignored.
    HRESULT Call(const std::string& service,
                 const std::string& method,
                 const google::protobuf::Message& request,
                 google::protobuf::Message* response = nullptr);

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

        // Serial ports (16550 UART COM ports, e.g. earlycon on port 0).
        struct SerialPort
        {
            uint32_t Port;          // 0-3 (COM1-COM4)
            std::string SocketPath; // Named pipe or Unix domain socket path
        };
        std::vector<SerialPort> SerialPorts;

        // Virtio console device (/dev/hvc0 in the guest).
        // Path to a named pipe or Unix domain socket for the console backend.
        std::string VirtioConsolePath;
    };

    // CreateVM: configure and create the VM (left in paused state).
    HRESULT CreateVm(const VmConfig& config);

    // ResumeVM: start a paused VM.
    HRESULT ResumeVm();

    // WaitVM: blocks until the VM halts or is torn down.
    HRESULT WaitVm();

    // TeardownVM: release all VM resources and unblock the WaitVM call.
    HRESULT TeardownVm();

private:
    // ttrpc service and method names (from vmservice.proto).
    static constexpr char c_serviceName[] = "vmservice.VM";
    static constexpr char c_createVmMethod[] = "CreateVM";
    static constexpr char c_resumeVmMethod[] = "ResumeVM";
    static constexpr char c_waitVmMethod[] = "WaitVM";
    static constexpr char c_teardownVmMethod[] = "TeardownVM";
    static constexpr char c_modifyResourceMethod[] = "ModifyResource";

    // Send a ttrpc request payload and wait for the response payload.
    // Returns S_OK on success, or an HRESULT error if the server returned a
    // status error or there was a communication failure.
    HRESULT SendRequest(const std::string& service,
                        const std::string& method,
                        const std::vector<uint8_t>& payload,
                        std::vector<uint8_t>* responsePayload);

    // Socket send/recv timeout to prevent indefinite blocking.
    static constexpr DWORD c_socketTimeoutMs = 30000;

    // Low-level socket I/O.
    HRESULT SendAll(const void* data, size_t length);
    HRESULT RecvAll(void* data, size_t length);

    std::recursive_mutex m_lock;
    SOCKET m_socket = INVALID_SOCKET;
    uint32_t m_nextStreamId = 1; // ttrpc client stream IDs must be odd
};

} // namespace wsl::windows::service::wslc
