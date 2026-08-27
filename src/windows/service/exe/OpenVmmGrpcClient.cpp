// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"

#include "OpenVmmGrpcClient.h"
#include <grpc/impl/channel_arg_names.h>
#include <algorithm>

namespace wsl::windows::service::wslc {

HRESULT OpenVmmGrpcClient::Connect(LPCWSTR socketPath, UINT32 timeoutMs)
{
    if (socketPath == nullptr)
    {
        return E_POINTER;
    }

    auto target = ToUtf8(socketPath);
    std::replace(target.begin(), target.end(), '\\', '/');
    m_timeout = std::chrono::milliseconds(timeoutMs);
    grpc::ChannelArguments channelArguments;
    channelArguments.SetString(GRPC_ARG_DEFAULT_AUTHORITY, "localhost");
    auto channel = grpc::CreateCustomChannel(
        "unix:" + target, grpc::InsecureChannelCredentials(), channelArguments);
    if (!channel->WaitForConnected(std::chrono::system_clock::now() + m_timeout))
    {
        return HRESULT_FROM_WIN32(WAIT_TIMEOUT);
    }

    m_stub = vmservice::VM::NewStub(std::move(channel));
    return S_OK;
}

HRESULT OpenVmmGrpcClient::Disconnect()
{
    m_stub.reset();
    return S_OK;
}

HRESULT OpenVmmGrpcClient::SetKernelPath(LPCWSTR path) { m_config.mutable_direct_boot()->set_kernel_path(ToUtf8(path)); return S_OK; }
HRESULT OpenVmmGrpcClient::SetInitrdPath(LPCWSTR path) { m_config.mutable_direct_boot()->set_initrd_path(ToUtf8(path)); return S_OK; }
HRESULT OpenVmmGrpcClient::SetKernelCmdLine(LPCWSTR commandLine) { m_config.mutable_direct_boot()->set_kernel_cmdline(ToUtf8(commandLine)); return S_OK; }
HRESULT OpenVmmGrpcClient::SetMemoryMb(UINT64 memoryMb) { m_config.mutable_memory_config()->set_memory_mb(memoryMb); return S_OK; }
HRESULT OpenVmmGrpcClient::SetProcessorCount(UINT32 count) { m_config.mutable_processor_config()->set_processor_count(count); return S_OK; }
HRESULT OpenVmmGrpcClient::SetHvSocketPath(LPCWSTR path) { m_config.mutable_hvsocket_config()->set_path(ToUtf8(path)); return S_OK; }

HRESULT OpenVmmGrpcClient::AddBootDisk(UINT32 controller, UINT32 lun, LPCWSTR hostPath, BOOL readOnly)
{
    auto disk = m_config.mutable_devices_config()->add_scsi_disks();
    disk->set_controller(controller);
    disk->set_lun(lun);
    disk->set_host_path(ToUtf8(hostPath));
    disk->set_type(vmservice::SCSI_DISK_TYPE_VHDX);
    disk->set_read_only(readOnly != FALSE);
    return S_OK;
}

HRESULT OpenVmmGrpcClient::SetConsommeNic(LPCWSTR nicId, LPCWSTR macAddress)
{
    m_nicId = ToUtf8(nicId);
    m_macAddress = ToUtf8(macAddress);
    auto nic = m_config.mutable_devices_config()->add_nic_config();
    nic->set_nic_id(m_nicId);
    nic->set_mac_address(m_macAddress);
    nic->mutable_consomme()->set_cidr("");
    return S_OK;
}

HRESULT OpenVmmGrpcClient::AddSerialPort(UINT32 port, LPCWSTR socketPath)
{
    auto config = m_config.mutable_serial_config()->add_ports();
    config->set_port(port);
    config->set_socket_path(ToUtf8(socketPath));
    config->set_connect(true);
    return S_OK;
}

HRESULT OpenVmmGrpcClient::SetVirtioConsolePath(LPCWSTR path)
{
    auto console = m_config.mutable_devices_config()->mutable_virtio_console();
    console->set_socket_path(ToUtf8(path));
    console->set_connect(true);
    return S_OK;
}

HRESULT OpenVmmGrpcClient::CreateVm()
{
    vmservice::CreateVMRequest request;
    request.mutable_config()->CopyFrom(m_config);
    return Call([&](grpc::ClientContext& context, google::protobuf::Empty& response) { return m_stub->CreateVM(&context, request, &response); });
}

HRESULT OpenVmmGrpcClient::ResumeVm() { return Call([&](grpc::ClientContext& context, google::protobuf::Empty& response) { return m_stub->ResumeVM(&context, google::protobuf::Empty{}, &response); }); }
HRESULT OpenVmmGrpcClient::TeardownVm() { return Call([&](grpc::ClientContext& context, google::protobuf::Empty& response) { return m_stub->TeardownVM(&context, google::protobuf::Empty{}, &response); }); }
HRESULT OpenVmmGrpcClient::Quit() { return Call([&](grpc::ClientContext& context, google::protobuf::Empty& response) { return m_stub->Quit(&context, google::protobuf::Empty{}, &response); }); }
HRESULT OpenVmmGrpcClient::AttachScsiDisk(UINT32 controller, UINT32 lun, LPCWSTR hostPath, BOOL readOnly) { return ModifyDisk(vmservice::ADD, controller, lun, hostPath, readOnly); }
HRESULT OpenVmmGrpcClient::DetachScsiDisk(UINT32 controller, UINT32 lun) { return ModifyDisk(vmservice::REMOVE, controller, lun, L"", FALSE); }
HRESULT OpenVmmGrpcClient::AddShare(LPCWSTR, LPCWSTR, BOOL) { return E_NOTIMPL; }
HRESULT OpenVmmGrpcClient::RemoveShare(LPCWSTR) { return E_NOTIMPL; }
HRESULT OpenVmmGrpcClient::BindPort(UINT16 hostPort, UINT16 guestPort, BOOL tcp, INT32) { return ModifyNic(vmservice::ADD, hostPort, guestPort, tcp); }
HRESULT OpenVmmGrpcClient::UnbindPort(UINT16 hostPort, UINT16 guestPort, BOOL tcp, INT32) { return ModifyNic(vmservice::REMOVE, hostPort, guestPort, tcp); }

std::string OpenVmmGrpcClient::ToUtf8(LPCWSTR value)
{
    if (value == nullptr)
    {
        return {};
    }

    const auto length = WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
    if (length == 0)
    {
        return {};
    }

    std::string result(length, '\0');
    WideCharToMultiByte(CP_UTF8, 0, value, -1, result.data(), length, nullptr, nullptr);
    result.pop_back();
    return result;
}

HRESULT OpenVmmGrpcClient::StatusToHresult(const grpc::Status& status)
{
    if (status.ok())
    {
        return S_OK;
    }

    WSL_LOG(
        "OpenVmmGrpcCallFailed",
        TraceLoggingValue(static_cast<INT32>(status.error_code()), "StatusCode"),
        TraceLoggingValue(status.error_message().c_str(), "StatusMessage"),
        TraceLoggingValue(status.error_details().c_str(), "StatusDetails"));

    return status.error_code() == grpc::StatusCode::DEADLINE_EXCEEDED ? HRESULT_FROM_WIN32(WAIT_TIMEOUT) : HRESULT_FROM_WIN32(ERROR_CONNECTION_ABORTED);
}

HRESULT OpenVmmGrpcClient::ModifyNic(vmservice::ModifyType type, UINT16 hostPort, UINT16 guestPort, BOOL tcp)
{
    if (m_nicId.empty())
    {
        return HRESULT_FROM_WIN32(ERROR_INVALID_STATE);
    }

    vmservice::ModifyResourceRequest request;
    request.set_type(type);
    auto nic = request.mutable_nic_config();
    nic->set_nic_id(m_nicId);
    nic->set_mac_address(m_macAddress);
    auto port = nic->mutable_consomme()->add_ports();
    port->set_host_port(hostPort);
    port->set_guest_port(guestPort);
    port->set_protocol(tcp ? vmservice::TCP : vmservice::UDP);
    return Call([&](grpc::ClientContext& context, google::protobuf::Empty& response) { return m_stub->ModifyResource(&context, request, &response); });
}

HRESULT OpenVmmGrpcClient::ModifyDisk(vmservice::ModifyType type, UINT32 controller, UINT32 lun, LPCWSTR hostPath, BOOL readOnly)
{
    vmservice::ModifyResourceRequest request;
    request.set_type(type);
    auto disk = request.mutable_scsi_disk();
    disk->set_controller(controller);
    disk->set_lun(lun);
    disk->set_host_path(ToUtf8(hostPath));
    disk->set_type(vmservice::SCSI_DISK_TYPE_VHDX);
    disk->set_read_only(readOnly != FALSE);
    return Call([&](grpc::ClientContext& context, google::protobuf::Empty& response) { return m_stub->ModifyResource(&context, request, &response); });
}

} // namespace wsl::windows::service::wslc