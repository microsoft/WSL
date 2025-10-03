/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    usbservice.hpp

Abstract:

    This file contains USB passthrough service function declarations.
    Provides USB device enumeration and passthrough over Hyper-V sockets.

--*/

#pragma once

#include <windows.h>
#include <setupapi.h>
#include <usbiodef.h>
#include <usb.h>
#include <usbioctl.h>
#include <usb200.h>
#include <usbspec.h>
#include <wil/resource.h>
#include <vector>
#include <string>
#include <memory>

namespace wsl::windows::common::usb {

// USB passthrough protocol constants
constexpr unsigned long USB_PASSTHROUGH_PORT = 0x5553422; // 'USB' in hex

// Protocol message types
enum class UsbMessageType : uint32_t {
    DeviceEnumeration = 1,
    DeviceAttach = 2,
    DeviceDetach = 3,
    UrbRequest = 4,
    UrbResponse = 5,
    DeviceEvent = 6,
    Error = 0xFF
};

// USB device info structure
struct UsbDeviceInfo {
    char InstanceId[256];
    char DeviceDesc[256];
    uint16_t VendorId;
    uint16_t ProductId;
    uint16_t BcdDevice;
    uint8_t DeviceClass;
    uint8_t DeviceSubClass;
    uint8_t DeviceProtocol;
    uint8_t ConfigurationCount;
    uint8_t CurrentConfiguration;
    bool IsAttached;
};

// Protocol message header
struct UsbMessageHeader {
    UsbMessageType Type;
    uint32_t PayloadSize;
    uint32_t SequenceNumber;
    uint32_t Reserved;
};

// Device enumeration request/response
struct UsbEnumerationRequest {
    uint32_t Reserved;
};

struct UsbEnumerationResponse {
    uint32_t DeviceCount;
    // Followed by DeviceCount * UsbDeviceInfo
};

// Device attach/detach messages
struct UsbAttachRequest {
    char InstanceId[256];
};

struct UsbAttachResponse {
    uint32_t Status; // 0 = success
    char ErrorMessage[256];
};

struct UsbDetachRequest {
    char InstanceId[256];
};

struct UsbDetachResponse {
    uint32_t Status;
};

// URB (USB Request Block) transfer
struct UsbUrbRequest {
    char InstanceId[256];
    uint16_t Function; // URB function code
    uint16_t Reserved;
    uint32_t Flags;
    uint32_t TransferBufferLength;
    uint8_t Endpoint;
    uint8_t Reserved2[3];
    // Followed by transfer buffer data
};

struct UsbUrbResponse {
    uint32_t Status;
    uint32_t TransferredLength;
    // Followed by response data
};

// USB Service class
class UsbService {
public:
    UsbService() = default;
    ~UsbService() = default;

    // Initialize the USB service
    HRESULT Initialize();

    // Shutdown the service
    void Shutdown();

    // Enumerate all USB devices
    std::vector<UsbDeviceInfo> EnumerateDevices();

    // Attach a USB device for passthrough
    HRESULT AttachDevice(_In_ const std::string& instanceId, _In_ SOCKET hvSocket);

    // Detach a USB device
    HRESULT DetachDevice(_In_ const std::string& instanceId);

    // Process URB requests from the guest
    HRESULT ProcessUrbRequest(_In_ const UsbUrbRequest& request, _Out_ UsbUrbResponse& response, _Out_ std::vector<uint8_t>& responseData);

    // Check if a device is currently attached
    bool IsDeviceAttached(_In_ const std::string& instanceId) const;

private:
    struct AttachedDevice {
        std::string InstanceId;
        wil::unique_hfile DeviceHandle;
        SOCKET Socket;
    };

    std::vector<AttachedDevice> m_attachedDevices;
    wil::critical_section m_lock;

    // Internal helpers
    HRESULT GetDeviceInfo(_In_ HDEVINFO deviceInfoSet, _In_ PSP_DEVINFO_DATA deviceInfoData, _Out_ UsbDeviceInfo& info);
    HRESULT OpenUsbDevice(_In_ const std::string& instanceId, _Out_ wil::unique_hfile& handle);
    HRESULT SendUrbToDevice(_In_ HANDLE deviceHandle, _In_ const UsbUrbRequest& request, _In_ const std::vector<uint8_t>& requestData, _Out_ UsbUrbResponse& response, _Out_ std::vector<uint8_t>& responseData);
};

// Protocol helper functions
HRESULT SendUsbMessage(_In_ SOCKET socket, _In_ UsbMessageType type, _In_reads_bytes_opt_(payloadSize) const void* payload, _In_ uint32_t payloadSize);
HRESULT ReceiveUsbMessage(_In_ SOCKET socket, _Out_ UsbMessageHeader& header, _Out_ std::vector<uint8_t>& payload);

} // namespace wsl::windows::common::usb
