/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    usbservice.cpp

Abstract:

    This file contains USB passthrough service implementation.
    Provides USB device enumeration and passthrough over Hyper-V sockets.

--*/

#include "precomp.h"
#include "usbservice.hpp"
#include <cfgmgr32.h>
#include <initguid.h>
#include <devguid.h>
#include <wil/result.h>

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "cfgmgr32.lib")

namespace wsl::windows::common::usb {

HRESULT UsbService::Initialize()
{
    return S_OK;
}

void UsbService::Shutdown()
{
    auto lock = m_lock.lock();
    m_attachedDevices.clear();
}

std::vector<UsbDeviceInfo> UsbService::EnumerateDevices()
{
    std::vector<UsbDeviceInfo> devices;

    // Get all USB devices
    wil::unique_hdevinfo deviceInfoSet(SetupDiGetClassDevs(
        &GUID_DEVINTERFACE_USB_DEVICE,
        nullptr,
        nullptr,
        DIGCF_PRESENT | DIGCF_DEVICEINTERFACE));

    RETURN_HR_IF(E_FAIL, !deviceInfoSet);

    SP_DEVINFO_DATA deviceInfoData{};
    deviceInfoData.cbSize = sizeof(deviceInfoData);

    for (DWORD i = 0; SetupDiEnumDeviceInfo(deviceInfoSet.get(), i, &deviceInfoData); i++)
    {
        UsbDeviceInfo info{};
        if (SUCCEEDED(GetDeviceInfo(deviceInfoSet.get(), &deviceInfoData, info)))
        {
            devices.push_back(info);
        }
    }

    return devices;
}

HRESULT UsbService::GetDeviceInfo(
    _In_ HDEVINFO deviceInfoSet,
    _In_ PSP_DEVINFO_DATA deviceInfoData,
    _Out_ UsbDeviceInfo& info)
{
    ZeroMemory(&info, sizeof(info));

    // Get instance ID
    DWORD requiredSize = 0;
    if (CM_Get_Device_ID_Size(&requiredSize, deviceInfoData->DevInst, 0) != CR_SUCCESS)
    {
        return E_FAIL;
    }

    std::vector<wchar_t> instanceIdW(requiredSize + 1);
    if (CM_Get_Device_IDW(deviceInfoData->DevInst, instanceIdW.data(), requiredSize + 1, 0) != CR_SUCCESS)
    {
        return E_FAIL;
    }

    // Convert to narrow string
    WideCharToMultiByte(CP_UTF8, 0, instanceIdW.data(), -1, info.InstanceId, sizeof(info.InstanceId), nullptr, nullptr);

    // Get device description
    BYTE buffer[512];
    DWORD dataType = 0;
    if (SetupDiGetDeviceRegistryPropertyW(
            deviceInfoSet,
            deviceInfoData,
            SPDRP_DEVICEDESC,
            &dataType,
            buffer,
            sizeof(buffer),
            &requiredSize))
    {
        WideCharToMultiByte(CP_UTF8, 0, (wchar_t*)buffer, -1, info.DeviceDesc, sizeof(info.DeviceDesc), nullptr, nullptr);
    }

    // Get hardware IDs to extract VID/PID
    if (SetupDiGetDeviceRegistryPropertyW(
            deviceInfoSet,
            deviceInfoData,
            SPDRP_HARDWAREID,
            &dataType,
            buffer,
            sizeof(buffer),
            &requiredSize))
    {
        // Parse hardware ID string (format: USB\VID_xxxx&PID_yyyy...)
        std::wstring hwId((wchar_t*)buffer);
        size_t vidPos = hwId.find(L"VID_");
        size_t pidPos = hwId.find(L"PID_");

        if (vidPos != std::wstring::npos && pidPos != std::wstring::npos)
        {
            info.VendorId = (uint16_t)wcstoul(hwId.substr(vidPos + 4, 4).c_str(), nullptr, 16);
            info.ProductId = (uint16_t)wcstoul(hwId.substr(pidPos + 4, 4).c_str(), nullptr, 16);
        }
    }

    // Check if currently attached
    auto lock = m_lock.lock();
    info.IsAttached = IsDeviceAttached(info.InstanceId);

    return S_OK;
}

HRESULT UsbService::OpenUsbDevice(_In_ const std::string& instanceId, _Out_ wil::unique_hfile& handle)
{
    // Get device interface path
    wil::unique_hdevinfo deviceInfoSet(SetupDiGetClassDevs(
        &GUID_DEVINTERFACE_USB_DEVICE,
        nullptr,
        nullptr,
        DIGCF_PRESENT | DIGCF_DEVICEINTERFACE));

    RETURN_HR_IF(E_FAIL, !deviceInfoSet);

    SP_DEVICE_INTERFACE_DATA interfaceData{};
    interfaceData.cbSize = sizeof(interfaceData);

    SP_DEVINFO_DATA deviceInfoData{};
    deviceInfoData.cbSize = sizeof(deviceInfoData);

    // Find the device with matching instance ID
    for (DWORD i = 0; SetupDiEnumDeviceInfo(deviceInfoSet.get(), i, &deviceInfoData); i++)
    {
        DWORD requiredSize = 0;
        CM_Get_Device_ID_Size(&requiredSize, deviceInfoData.DevInst, 0);

        std::vector<wchar_t> currentInstanceId(requiredSize + 1);
        if (CM_Get_Device_IDW(deviceInfoData.DevInst, currentInstanceId.data(), requiredSize + 1, 0) == CR_SUCCESS)
        {
            char narrowId[256];
            WideCharToMultiByte(CP_UTF8, 0, currentInstanceId.data(), -1, narrowId, sizeof(narrowId), nullptr, nullptr);

            if (_stricmp(narrowId, instanceId.c_str()) == 0)
            {
                // Get device interface detail
                if (SetupDiEnumDeviceInterfaces(deviceInfoSet.get(), &deviceInfoData, &GUID_DEVINTERFACE_USB_DEVICE, 0, &interfaceData))
                {
                    DWORD detailSize = 0;
                    SetupDiGetDeviceInterfaceDetailW(deviceInfoSet.get(), &interfaceData, nullptr, 0, &detailSize, nullptr);

                    std::vector<uint8_t> detailBuffer(detailSize);
                    auto* detail = reinterpret_cast<PSP_DEVICE_INTERFACE_DETAIL_DATA_W>(detailBuffer.data());
                    detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);

                    if (SetupDiGetDeviceInterfaceDetailW(deviceInfoSet.get(), &interfaceData, detail, detailSize, nullptr, nullptr))
                    {
                        // Open the device
                        handle.reset(CreateFileW(
                            detail->DevicePath,
                            GENERIC_READ | GENERIC_WRITE,
                            FILE_SHARE_READ | FILE_SHARE_WRITE,
                            nullptr,
                            OPEN_EXISTING,
                            FILE_FLAG_OVERLAPPED,
                            nullptr));

                        RETURN_LAST_ERROR_IF(!handle);
                        return S_OK;
                    }
                }
            }
        }
    }

    return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
}

HRESULT UsbService::AttachDevice(_In_ const std::string& instanceId, _In_ SOCKET hvSocket)
{
    auto lock = m_lock.lock();

    // Check if already attached
    if (IsDeviceAttached(instanceId))
    {
        return HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS);
    }

    // Open the USB device
    wil::unique_hfile deviceHandle;
    RETURN_IF_FAILED(OpenUsbDevice(instanceId, deviceHandle));

    // Add to attached devices list
    AttachedDevice attached;
    attached.InstanceId = instanceId;
    attached.DeviceHandle = std::move(deviceHandle);
    attached.Socket = hvSocket;

    m_attachedDevices.push_back(std::move(attached));

    return S_OK;
}

HRESULT UsbService::DetachDevice(_In_ const std::string& instanceId)
{
    auto lock = m_lock.lock();

    auto it = std::find_if(m_attachedDevices.begin(), m_attachedDevices.end(),
        [&instanceId](const AttachedDevice& device) {
            return device.InstanceId == instanceId;
        });

    if (it == m_attachedDevices.end())
    {
        return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    }

    m_attachedDevices.erase(it);
    return S_OK;
}

bool UsbService::IsDeviceAttached(_In_ const std::string& instanceId) const
{
    return std::any_of(m_attachedDevices.begin(), m_attachedDevices.end(),
        [&instanceId](const AttachedDevice& device) {
            return device.InstanceId == instanceId;
        });
}

HRESULT UsbService::ProcessUrbRequest(
    _In_ const UsbUrbRequest& request,
    _Out_ UsbUrbResponse& response,
    _Out_ std::vector<uint8_t>& responseData)
{
    auto lock = m_lock.lock();

    // Find the attached device
    auto it = std::find_if(m_attachedDevices.begin(), m_attachedDevices.end(),
        [&request](const AttachedDevice& device) {
            return device.InstanceId == request.InstanceId;
        });

    if (it == m_attachedDevices.end())
    {
        response.Status = ERROR_NOT_FOUND;
        response.TransferredLength = 0;
        return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    }

    // Allocate URB buffer - use maximum size to accommodate all URB types
    std::vector<uint8_t> urbBuffer(sizeof(struct _URB_CONTROL_TRANSFER_EX));
    struct _URB_HEADER* urbHeader = reinterpret_cast<struct _URB_HEADER*>(urbBuffer.data());
    
    // Allocate transfer buffer
    responseData.resize(request.TransferBufferLength);

    // Build URB based on function code
    switch (request.Function)
    {
        case URB_FUNCTION_BULK_OR_INTERRUPT_TRANSFER:
        {
            auto* urb = reinterpret_cast<struct _URB_BULK_OR_INTERRUPT_TRANSFER*>(urbBuffer.data());
            urb->Hdr.Length = sizeof(struct _URB_BULK_OR_INTERRUPT_TRANSFER);
            urb->Hdr.Function = URB_FUNCTION_BULK_OR_INTERRUPT_TRANSFER;
            urb->PipeHandle = reinterpret_cast<USBD_PIPE_HANDLE>(static_cast<ULONG_PTR>(request.Endpoint));
            urb->TransferFlags = request.Flags;
            urb->TransferBufferLength = request.TransferBufferLength;
            urb->TransferBuffer = responseData.data();
            urb->TransferBufferMDL = nullptr;
            urb->UrbLink = nullptr;
            break;
        }

        case URB_FUNCTION_CONTROL_TRANSFER:
        case URB_FUNCTION_CONTROL_TRANSFER_EX:
        {
            auto* urb = reinterpret_cast<struct _URB_CONTROL_TRANSFER*>(urbBuffer.data());
            urb->Hdr.Length = sizeof(struct _URB_CONTROL_TRANSFER);
            urb->Hdr.Function = request.Function;
            urb->PipeHandle = reinterpret_cast<USBD_PIPE_HANDLE>(static_cast<ULONG_PTR>(request.Endpoint));
            urb->TransferFlags = request.Flags;
            urb->TransferBufferLength = request.TransferBufferLength;
            urb->TransferBuffer = responseData.data();
            urb->TransferBufferMDL = nullptr;
            urb->UrbLink = nullptr;
            // Setup packet would be extracted from request payload
            ZeroMemory(&urb->SetupPacket, sizeof(urb->SetupPacket));
            break;
        }

        case URB_FUNCTION_ISOCH_TRANSFER:
        {
            auto* urb = reinterpret_cast<struct _URB_ISOCH_TRANSFER*>(urbBuffer.data());
            urb->Hdr.Length = sizeof(struct _URB_ISOCH_TRANSFER);
            urb->Hdr.Function = URB_FUNCTION_ISOCH_TRANSFER;
            urb->PipeHandle = reinterpret_cast<USBD_PIPE_HANDLE>(static_cast<ULONG_PTR>(request.Endpoint));
            urb->TransferFlags = request.Flags;
            urb->TransferBufferLength = request.TransferBufferLength;
            urb->TransferBuffer = responseData.data();
            urb->TransferBufferMDL = nullptr;
            urb->UrbLink = nullptr;
            urb->NumberOfPackets = 0; // Would be extracted from request
            break;
        }

        case URB_FUNCTION_GET_DESCRIPTOR_FROM_DEVICE:
        case URB_FUNCTION_GET_DESCRIPTOR_FROM_INTERFACE:
        case URB_FUNCTION_GET_DESCRIPTOR_FROM_ENDPOINT:
        {
            auto* urb = reinterpret_cast<struct _URB_CONTROL_DESCRIPTOR_REQUEST*>(urbBuffer.data());
            urb->Hdr.Length = sizeof(struct _URB_CONTROL_DESCRIPTOR_REQUEST);
            urb->Hdr.Function = request.Function;
            urb->TransferBufferLength = request.TransferBufferLength;
            urb->TransferBuffer = responseData.data();
            urb->TransferBufferMDL = nullptr;
            urb->UrbLink = nullptr;
            // Descriptor type, index, language ID would be extracted from request
            urb->Index = 0;
            urb->DescriptorType = 0;
            urb->LanguageId = 0;
            break;
        }

        case URB_FUNCTION_SELECT_CONFIGURATION:
        {
            auto* urb = reinterpret_cast<struct _URB_SELECT_CONFIGURATION*>(urbBuffer.data());
            urb->Hdr.Length = sizeof(struct _URB_SELECT_CONFIGURATION);
            urb->Hdr.Function = URB_FUNCTION_SELECT_CONFIGURATION;
            urb->ConfigurationDescriptor = nullptr; // Would point to config descriptor
            urb->UrbLink = nullptr;
            break;
        }

        case URB_FUNCTION_SELECT_INTERFACE:
        {
            auto* urb = reinterpret_cast<struct _URB_SELECT_INTERFACE*>(urbBuffer.data());
            urb->Hdr.Length = sizeof(struct _URB_SELECT_INTERFACE);
            urb->Hdr.Function = URB_FUNCTION_SELECT_INTERFACE;
            urb->ConfigurationHandle = nullptr; // Would be extracted from request
            urb->UrbLink = nullptr;
            break;
        }

        case URB_FUNCTION_ABORT_PIPE:
        case URB_FUNCTION_RESET_PIPE:
        {
            auto* urb = reinterpret_cast<struct _URB_PIPE_REQUEST*>(urbBuffer.data());
            urb->Hdr.Length = sizeof(struct _URB_PIPE_REQUEST);
            urb->Hdr.Function = request.Function;
            urb->PipeHandle = reinterpret_cast<USBD_PIPE_HANDLE>(static_cast<ULONG_PTR>(request.Endpoint));
            urb->Reserved = 0;
            break;
        }

        default:
            response.Status = ERROR_NOT_SUPPORTED;
            response.TransferredLength = 0;
            return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
    }

    // Submit URB to USB device via IOCTL
    DWORD bytesReturned = 0;
    BOOL success = DeviceIoControl(
        it->DeviceHandle.get(),
        IOCTL_INTERNAL_USB_SUBMIT_URB,
        urbBuffer.data(),
        static_cast<DWORD>(urbBuffer.size()),
        urbBuffer.data(),
        static_cast<DWORD>(urbBuffer.size()),
        &bytesReturned,
        nullptr);

    if (!success)
    {
        DWORD error = GetLastError();
        response.Status = error;
        response.TransferredLength = 0;
        responseData.clear();
        return HRESULT_FROM_WIN32(error);
    }

    // Extract results from URB based on function
    USBD_STATUS usbStatus = urbHeader->Status;
    response.Status = USBD_SUCCESS(usbStatus) ? ERROR_SUCCESS : ERROR_GEN_FAILURE;

    // Get transferred length based on URB type
    if (request.Function == URB_FUNCTION_BULK_OR_INTERRUPT_TRANSFER ||
        request.Function == URB_FUNCTION_CONTROL_TRANSFER ||
        request.Function == URB_FUNCTION_CONTROL_TRANSFER_EX)
    {
        auto* urb = reinterpret_cast<struct _URB_BULK_OR_INTERRUPT_TRANSFER*>(urbBuffer.data());
        response.TransferredLength = urb->TransferBufferLength;
    }
    else if (request.Function == URB_FUNCTION_ISOCH_TRANSFER)
    {
        auto* urb = reinterpret_cast<struct _URB_ISOCH_TRANSFER*>(urbBuffer.data());
        response.TransferredLength = urb->TransferBufferLength;
    }
    else
    {
        response.TransferredLength = 0;
    }

    // Resize response data to actual transferred length
    if (response.TransferredLength > 0 && (request.Flags & USBD_TRANSFER_DIRECTION_IN))
    {
        responseData.resize(response.TransferredLength);
    }
    else
    {
        responseData.clear();
    }

    return S_OK;
}

HRESULT SendUsbMessage(
    _In_ SOCKET socket,
    _In_ UsbMessageType type,
    _In_reads_bytes_opt_(payloadSize) const void* payload,
    _In_ uint32_t payloadSize)
{
    UsbMessageHeader header{};
    header.Type = type;
    header.PayloadSize = payloadSize;
    header.SequenceNumber = 0; // Should be tracked
    header.Reserved = 0;

    // Send header
    int result = send(socket, reinterpret_cast<const char*>(&header), sizeof(header), 0);
    RETURN_HR_IF(HRESULT_FROM_WIN32(WSAGetLastError()), result != sizeof(header));

    // Send payload if present
    if (payloadSize > 0 && payload != nullptr)
    {
        result = send(socket, reinterpret_cast<const char*>(payload), payloadSize, 0);
        RETURN_HR_IF(HRESULT_FROM_WIN32(WSAGetLastError()), result != payloadSize);
    }

    return S_OK;
}

HRESULT ReceiveUsbMessage(
    _In_ SOCKET socket,
    _Out_ UsbMessageHeader& header,
    _Out_ std::vector<uint8_t>& payload)
{
    // Receive header
    int result = recv(socket, reinterpret_cast<char*>(&header), sizeof(header), MSG_WAITALL);
    RETURN_HR_IF(HRESULT_FROM_WIN32(WSAGetLastError()), result != sizeof(header));

    // Receive payload if present
    if (header.PayloadSize > 0)
    {
        payload.resize(header.PayloadSize);
        result = recv(socket, reinterpret_cast<char*>(payload.data()), header.PayloadSize, MSG_WAITALL);
        RETURN_HR_IF(HRESULT_FROM_WIN32(WSAGetLastError()), result != header.PayloadSize);
    }
    else
    {
        payload.clear();
    }

    return S_OK;
}

} // namespace wsl::windows::common::usb
