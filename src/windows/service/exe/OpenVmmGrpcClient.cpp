// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"
#include "OpenVmmGrpcClient.h"

#include <grpc/impl/channel_arg_names.h>

#include <algorithm>

using wsl::windows::service::OpenVmmGrpcClient;
using wsl::shared::string::WideToMultiByte;

namespace {
vmservice::DiskType GetDiskType(_In_ LPCWSTR Path)
{
    const auto extension = std::filesystem::path{Path}.extension().native();
    return wsl::windows::common::string::IsPathComponentEqual(extension, L".vhdx") ? vmservice::SCSI_DISK_TYPE_VHDX :
                                                                                     vmservice::SCSI_DISK_TYPE_VHD1;
}
}

HRESULT OpenVmmGrpcClient::Connect(_In_ LPCWSTR SocketPath, _In_ UINT32 TimeoutMs)
{
    RETURN_HR_IF(E_POINTER, SocketPath == nullptr);

    auto target = WideToMultiByte(SocketPath);
    std::replace(target.begin(), target.end(), '\\', '/');
    m_timeout = std::chrono::milliseconds(TimeoutMs);

    grpc::ChannelArguments channelArguments;
    channelArguments.SetString(GRPC_ARG_DEFAULT_AUTHORITY, "localhost");
    auto channel = grpc::CreateCustomChannel("unix:" + target, grpc::InsecureChannelCredentials(), channelArguments);
    RETURN_HR_IF(HRESULT_FROM_WIN32(WAIT_TIMEOUT), !channel->WaitForConnected(std::chrono::system_clock::now() + m_timeout));

    m_stub = vmservice::VM::NewStub(std::move(channel));
    return S_OK;
}

HRESULT OpenVmmGrpcClient::Disconnect()
{
    m_stub.reset();
    return S_OK;
}

HRESULT OpenVmmGrpcClient::SetKernelPath(_In_ LPCWSTR Path)
{
    m_config.mutable_direct_boot()->set_kernel_path(WideToMultiByte(Path));
    return S_OK;
}

HRESULT OpenVmmGrpcClient::SetInitrdPath(_In_ LPCWSTR Path)
{
    m_config.mutable_direct_boot()->set_initrd_path(WideToMultiByte(Path));
    return S_OK;
}

HRESULT OpenVmmGrpcClient::SetKernelCmdLine(_In_ LPCWSTR CommandLine)
{
    m_config.mutable_direct_boot()->set_kernel_cmdline(WideToMultiByte(CommandLine));
    return S_OK;
}

HRESULT OpenVmmGrpcClient::SetMemoryMb(_In_ UINT64 MemoryMb)
{
    m_config.mutable_memory_config()->set_memory_mb(MemoryMb);
    return S_OK;
}

HRESULT OpenVmmGrpcClient::SetProcessorCount(_In_ UINT32 Count)
{
    m_config.mutable_processor_config()->set_processor_count(Count);
    return S_OK;
}

HRESULT OpenVmmGrpcClient::SetHvSocketPath(_In_ LPCWSTR Path)
{
    m_config.mutable_hvsocket_config()->set_path(WideToMultiByte(Path));
    return S_OK;
}

HRESULT OpenVmmGrpcClient::AddBootDisk(_In_ UINT32 Controller, _In_ UINT32 Lun, _In_ LPCWSTR HostPath, _In_ BOOL ReadOnly)
{
    auto disk = m_config.mutable_devices_config()->add_scsi_disks();
    disk->set_controller(Controller);
    disk->set_lun(Lun);
    disk->set_host_path(WideToMultiByte(HostPath));
    disk->set_type(GetDiskType(HostPath));
    disk->set_read_only(ReadOnly != FALSE);
    return S_OK;
}

HRESULT OpenVmmGrpcClient::SetConsommeNic(_In_ LPCWSTR NicId, _In_ LPCWSTR MacAddress)
{
    m_nicId = WideToMultiByte(NicId);
    m_macAddress = WideToMultiByte(MacAddress);

    auto nic = m_config.mutable_devices_config()->add_nic_config();
    nic->set_nic_id(m_nicId);
    nic->set_mac_address(m_macAddress);
    nic->mutable_consomme()->set_cidr("");
    return S_OK;
}

HRESULT OpenVmmGrpcClient::AddSerialPort(_In_ UINT32 Port, _In_ LPCWSTR SocketPath)
{
    auto config = m_config.mutable_serial_config()->add_ports();
    config->set_port(Port);
    config->set_socket_path(WideToMultiByte(SocketPath));
    config->set_connect(true);
    return S_OK;
}

HRESULT OpenVmmGrpcClient::SetVirtioConsolePath(_In_ LPCWSTR Path)
{
    auto console = m_config.mutable_devices_config()->mutable_virtio_console();
    console->set_socket_path(WideToMultiByte(Path));
    console->set_connect(true);
    return S_OK;
}

HRESULT OpenVmmGrpcClient::CreateVm()
{
    vmservice::CreateVMRequest request;
    request.mutable_config()->CopyFrom(m_config);
    return Call([&](grpc::ClientContext& Context, google::protobuf::Empty& Response) {
        return m_stub->CreateVM(&Context, request, &Response);
    });
}

HRESULT OpenVmmGrpcClient::ResumeVm()
{
    return Call([&](grpc::ClientContext& Context, google::protobuf::Empty& Response) {
        return m_stub->ResumeVM(&Context, google::protobuf::Empty{}, &Response);
    });
}

HRESULT OpenVmmGrpcClient::TeardownVm()
{
    return Call([&](grpc::ClientContext& Context, google::protobuf::Empty& Response) {
        return m_stub->TeardownVM(&Context, google::protobuf::Empty{}, &Response);
    });
}

HRESULT OpenVmmGrpcClient::Quit()
{
    return Call([&](grpc::ClientContext& Context, google::protobuf::Empty& Response) {
        return m_stub->Quit(&Context, google::protobuf::Empty{}, &Response);
    });
}

HRESULT OpenVmmGrpcClient::AttachScsiDisk(
    _In_ UINT32 Controller, _In_ UINT32 Lun, _In_ LPCWSTR HostPath, _In_ BOOL ReadOnly)
{
    return ModifyDisk(vmservice::ADD, Controller, Lun, HostPath, ReadOnly);
}

HRESULT OpenVmmGrpcClient::DetachScsiDisk(_In_ UINT32 Controller, _In_ UINT32 Lun)
{
    return ModifyDisk(vmservice::REMOVE, Controller, Lun, L"", FALSE);
}

HRESULT OpenVmmGrpcClient::AddVirtioFsDevice(_In_ LPCWSTR Tag, _In_ LPCWSTR RootPath, _Out_ GUID* InstanceId)
{
    RETURN_HR_IF(E_POINTER, Tag == nullptr || RootPath == nullptr || InstanceId == nullptr);

    vmservice::AddVpciDeviceRequest request;
    auto virtioFs = request.mutable_device()->mutable_virtio()->mutable_fs();
    virtioFs->set_tag(WideToMultiByte(Tag));
    virtioFs->set_root_path(WideToMultiByte(RootPath));

    vmservice::AddVpciDeviceResponse response;
    RETURN_IF_FAILED(Call(response, [&](grpc::ClientContext& Context, vmservice::AddVpciDeviceResponse& Response) {
        return m_stub->AddVpciDevice(&Context, request, &Response);
    }));

    const auto instanceId = wsl::shared::string::ToGuid(response.instance_id());
    RETURN_HR_IF(E_UNEXPECTED, !instanceId.has_value());
    *InstanceId = instanceId.value();
    return S_OK;
}

HRESULT OpenVmmGrpcClient::RemoveVpciDevice(_In_ const GUID& InstanceId)
{
    vmservice::RemoveVpciDeviceRequest request;
    request.set_instance_id(WideToMultiByte(
        wsl::shared::string::GuidToString<wchar_t>(InstanceId, wsl::shared::string::GuidToStringFlags::None)));

    return Call([&](grpc::ClientContext& Context, google::protobuf::Empty& Response) {
        return m_stub->RemoveVpciDevice(&Context, request, &Response);
    });
}

HRESULT OpenVmmGrpcClient::BindPort(_In_ UINT16 HostPort, _In_ UINT16 GuestPort, _In_ BOOL Tcp, _In_ INT32)
{
    return ModifyNic(vmservice::ADD, HostPort, GuestPort, Tcp);
}

HRESULT OpenVmmGrpcClient::UnbindPort(_In_ UINT16 HostPort, _In_ UINT16 GuestPort, _In_ BOOL Tcp, _In_ INT32)
{
    return ModifyNic(vmservice::REMOVE, HostPort, GuestPort, Tcp);
}

HRESULT OpenVmmGrpcClient::StatusToHresult(const grpc::Status& Status)
{
    if (Status.ok())
    {
        return S_OK;
    }

    WSL_LOG(
        "OpenVmmGrpcCallFailed",
        TraceLoggingValue(static_cast<INT32>(Status.error_code()), "StatusCode"),
        TraceLoggingValue(Status.error_message().c_str(), "StatusMessage"),
        TraceLoggingValue(Status.error_details().c_str(), "StatusDetails"));

    return Status.error_code() == grpc::StatusCode::DEADLINE_EXCEEDED ? HRESULT_FROM_WIN32(WAIT_TIMEOUT) :
                                                                       HRESULT_FROM_WIN32(ERROR_CONNECTION_ABORTED);
}

HRESULT OpenVmmGrpcClient::ModifyNic(vmservice::ModifyType Type, UINT16 HostPort, UINT16 GuestPort, BOOL Tcp)
{
    RETURN_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), m_nicId.empty());

    vmservice::ModifyResourceRequest request;
    request.set_type(Type);
    auto nic = request.mutable_nic_config();
    nic->set_nic_id(m_nicId);
    nic->set_mac_address(m_macAddress);
    auto port = nic->mutable_consomme()->add_ports();
    port->set_host_port(HostPort);
    port->set_guest_port(GuestPort);
    port->set_protocol(Tcp ? vmservice::TCP : vmservice::UDP);

    return Call([&](grpc::ClientContext& Context, google::protobuf::Empty& Response) {
        return m_stub->ModifyResource(&Context, request, &Response);
    });
}

HRESULT OpenVmmGrpcClient::ModifyDisk(
    vmservice::ModifyType Type, UINT32 Controller, UINT32 Lun, LPCWSTR HostPath, BOOL ReadOnly)
{
    vmservice::ModifyResourceRequest request;
    request.set_type(Type);
    auto disk = request.mutable_scsi_disk();
    disk->set_controller(Controller);
    disk->set_lun(Lun);
    disk->set_host_path(WideToMultiByte(HostPath));
    disk->set_type(GetDiskType(HostPath));
    disk->set_read_only(ReadOnly != FALSE);

    return Call([&](grpc::ClientContext& Context, google::protobuf::Empty& Response) {
        return m_stub->ModifyResource(&Context, request, &Response);
    });
}