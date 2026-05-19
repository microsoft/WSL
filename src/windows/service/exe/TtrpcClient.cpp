// Copyright (C) Microsoft Corporation. All rights reserved.

/*++

Module Name:

    TtrpcClient.cpp

Abstract:

    Minimal ttrpc client for communicating with OpenVMM's vmservice.

    This implementation uses generated protobuf types from VMService.proto for
    vmservice payloads, while keeping a generic ttrpc transport layer that can
    send any protobuf request/response pair.

    Wire format reference: openvmm/support/mesh/mesh_rpc/src/message.rs

--*/

#include "precomp.h"

#include "TtrpcClient.h"
#include "TtrpcEnvelopeCodec.h"

#include <afunix.h>

#include "VMService.pb.h"
#include "google/protobuf/empty.pb.h"
#include "stringshared.h"

using namespace wsl::windows::service::wslc;
using wsl::windows::service::wslc::detail::TtrpcEnvelopeCodec;

namespace
{
HRESULT DeserializeMessage(const std::vector<uint8_t>& bytes, google::protobuf::Message* message)
{
    RETURN_HR_IF(E_POINTER, message == nullptr);

    if (bytes.empty())
    {
        message->Clear();
        return S_OK;
    }

    RETURN_HR_IF_MSG(
        E_FAIL,
        !message->ParseFromArray(bytes.data(), static_cast<int>(bytes.size())),
        "ttrpc: failed to parse protobuf response payload");

    return S_OK;
}

HRESULT SerializeMessage(const google::protobuf::Message& message, std::vector<uint8_t>& bytes)
{
    std::string serialized;
    RETURN_HR_IF_MSG(E_FAIL, !message.SerializeToString(&serialized), "ttrpc: failed to serialize protobuf request payload");

    bytes.assign(serialized.begin(), serialized.end());
    return S_OK;
}

HRESULT GrpcStatusToHresult(int32_t statusCode)
{
    switch (statusCode)
    {
    case 3:
        return E_INVALIDARG;
    case 5:
        return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    case 8:
        return HRESULT_FROM_WIN32(ERROR_NO_SYSTEM_RESOURCES);
    case 12:
        return E_NOTIMPL;
    default:
        return E_FAIL;
    }
}
} // namespace

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
        return S_OK;
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

HRESULT TtrpcClient::Call(
    const std::string& service,
    const std::string& method,
    const google::protobuf::Message& request,
    google::protobuf::Message* response)
{
    std::vector<uint8_t> requestPayload;
    RETURN_IF_FAILED(SerializeMessage(request, requestPayload));

    std::vector<uint8_t> responsePayload;
    RETURN_IF_FAILED(SendRequest(service, method, requestPayload, &responsePayload));

    if (response != nullptr)
    {
        RETURN_IF_FAILED(DeserializeMessage(responsePayload, response));
    }

    return S_OK;
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

    vmservice::ModifyResourceRequest request;
    request.set_type(vmservice::ADD);

    auto* scsiDisk = request.mutable_scsi_disk();
    scsiDisk->set_controller(controller);
    scsiDisk->set_lun(lun);
    scsiDisk->set_host_path(hostPath);
    scsiDisk->set_type(vmservice::SCSI_DISK_TYPE_VHDX);
    scsiDisk->set_read_only(readOnly);

    google::protobuf::Empty response;
    return Call(c_serviceName, c_modifyResourceMethod, request, &response);
}
CATCH_RETURN()

HRESULT TtrpcClient::DetachScsiDisk(uint32_t controller, uint32_t lun)
try
{
    WSL_LOG(
        "TtrpcDetachScsiDisk",
        TraceLoggingValue(controller, "Controller"),
        TraceLoggingValue(lun, "Lun"));

    vmservice::ModifyResourceRequest request;
    request.set_type(vmservice::REMOVE);

    auto* scsiDisk = request.mutable_scsi_disk();
    scsiDisk->set_controller(controller);
    scsiDisk->set_lun(lun);

    google::protobuf::Empty response;
    return Call(c_serviceName, c_modifyResourceMethod, request, &response);
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

    vmservice::CreateVMRequest request;
    auto* vmConfig = request.mutable_config();

    vmConfig->mutable_memory_config()->set_memory_mb(config.MemoryMb);
    vmConfig->mutable_processor_config()->set_processor_count(config.ProcessorCount);

    for (const auto& disk : config.ScsiDisks)
    {
        auto* scsiDisk = vmConfig->mutable_devices_config()->add_scsi_disks();
        scsiDisk->set_controller(disk.Controller);
        scsiDisk->set_lun(disk.Lun);
        scsiDisk->set_host_path(disk.HostPath);
        scsiDisk->set_type(vmservice::SCSI_DISK_TYPE_VHDX);
        scsiDisk->set_read_only(disk.ReadOnly);
    }

    if (config.Nic.has_value())
    {
        auto* nicConfig = vmConfig->mutable_devices_config()->add_nic_config();
        nicConfig->set_nic_id(config.Nic->NicId);
        nicConfig->set_mac_address(config.Nic->MacAddress);
        // Set ConsommeBackend as the active oneof choice. The CIDR field is optional
        // and will use OpenVMM's default if empty.
        nicConfig->mutable_consomme()->set_cidr("");
    }

    if (!config.VirtioConsolePath.empty())
    {
        auto* virtioConsole = vmConfig->mutable_devices_config()->mutable_virtio_console();
        virtioConsole->set_socket_path(config.VirtioConsolePath);
        virtioConsole->set_connect(true);
    }

    for (const auto& serialPort : config.SerialPorts)
    {
        auto* portConfig = vmConfig->mutable_serial_config()->add_ports();
        portConfig->set_port(serialPort.Port);
        portConfig->set_socket_path(serialPort.SocketPath);
        portConfig->set_connect(true);
    }

    auto* directBoot = vmConfig->mutable_direct_boot();
    directBoot->set_kernel_path(config.KernelPath);
    directBoot->set_initrd_path(config.InitrdPath);
    directBoot->set_kernel_cmdline(config.KernelCmdLine);

    vmConfig->mutable_hvsocket_config()->set_path(config.HvSocketPath);

    google::protobuf::Empty response;
    return Call(c_serviceName, c_createVmMethod, request, &response);
}
CATCH_RETURN()

HRESULT TtrpcClient::ResumeVm()
try
{
    WSL_LOG("TtrpcResumeVm");

    google::protobuf::Empty request;
    google::protobuf::Empty response;
    return Call(c_serviceName, c_resumeVmMethod, request, &response);
}
CATCH_RETURN()

HRESULT TtrpcClient::WaitVm()
try
{
    WSL_LOG("TtrpcWaitVm");

    google::protobuf::Empty request;
    google::protobuf::Empty response;
    return Call(c_serviceName, c_waitVmMethod, request, &response);
}
CATCH_RETURN()

HRESULT TtrpcClient::TeardownVm()
try
{
    WSL_LOG("TtrpcTeardownVm");

    google::protobuf::Empty request;
    google::protobuf::Empty response;
    return Call(c_serviceName, c_teardownVmMethod, request, &response);
}
CATCH_RETURN()

HRESULT TtrpcClient::SendRequest(
    const std::string& service,
    const std::string& method,
    const std::vector<uint8_t>& payload,
    std::vector<uint8_t>* responsePayload)
{
    std::lock_guard lock(m_lock);

    RETURN_HR_IF(HRESULT_FROM_WIN32(ERROR_NOT_CONNECTED), m_socket == INVALID_SOCKET);

    auto ttrpcPayload = TtrpcEnvelopeCodec::EncodeRequestEnvelope(service, method, payload);

    detail::TtrpcMessageHeader header{};
    TtrpcEnvelopeCodec::WriteBigEndian32(header.Length, static_cast<uint32_t>(ttrpcPayload.size()));
    TtrpcEnvelopeCodec::WriteBigEndian32(header.StreamId, m_nextStreamId);
    header.MessageType = TtrpcEnvelopeCodec::c_messageTypeRequest;
    header.Flags = 0;

    uint32_t expectedStreamId = m_nextStreamId;
    m_nextStreamId += 2;

    RETURN_IF_FAILED(SendAll(&header, sizeof(header)));
    RETURN_IF_FAILED(SendAll(ttrpcPayload.data(), ttrpcPayload.size()));

    detail::TtrpcMessageHeader responseHeader{};
    RETURN_IF_FAILED(RecvAll(&responseHeader, sizeof(responseHeader)));

    RETURN_HR_IF_MSG(
        E_FAIL,
        responseHeader.MessageType != TtrpcEnvelopeCodec::c_messageTypeResponse,
        "ttrpc: expected response (type 2), got type %d",
        responseHeader.MessageType);

    uint32_t responseStreamId = TtrpcEnvelopeCodec::ReadBigEndian32(responseHeader.StreamId);
    RETURN_HR_IF_MSG(
        E_FAIL,
        responseStreamId != expectedStreamId,
        "ttrpc: stream ID mismatch: expected %u, got %u",
        expectedStreamId,
        responseStreamId);

    uint32_t responseLength = TtrpcEnvelopeCodec::ReadBigEndian32(responseHeader.Length);
    RETURN_HR_IF_MSG(
        E_FAIL,
        responseLength > TtrpcEnvelopeCodec::c_maxMessageBytes,
        "ttrpc: response too large: %u bytes",
        responseLength);

    std::vector<uint8_t> responseData(responseLength);
    if (responseLength > 0)
    {
        RETURN_IF_FAILED(RecvAll(responseData.data(), responseLength));
    }

    TtrpcEnvelopeCodec::DecodedResponse decodedResponse;
    RETURN_IF_FAILED(TtrpcEnvelopeCodec::DecodeResponseEnvelope(responseData, decodedResponse));

    if (decodedResponse.HasStatus && decodedResponse.StatusCode != 0)
    {
        WSL_LOG(
            "TtrpcRequestFailed",
            TraceLoggingValue(decodedResponse.StatusCode, "GrpcCode"),
            TraceLoggingValue(decodedResponse.StatusMessage.c_str(), "Message"));

        return GrpcStatusToHresult(decodedResponse.StatusCode);
    }

    if (responsePayload != nullptr)
    {
        *responsePayload = std::move(decodedResponse.Payload);
    }

    return S_OK;
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
