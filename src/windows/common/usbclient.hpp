/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    usbclient.hpp

Abstract:

    This file contains USB command-line interface declarations for wsl.exe.
    Provides commands for USB device management: --usb-list, --usb-attach, --usb-detach.

--*/

#pragma once

#include <string>
#include <vector>

namespace wsl::windows::common {

class UsbClient {
public:
    // USB CLI commands
    static int ListUsbDevices(_In_ bool verbose);
    static int AttachUsbDevice(_In_ const std::wstring& deviceId, _In_opt_ const std::wstring& distribution);
    static int DetachUsbDevice(_In_ const std::wstring& deviceId, _In_opt_ const std::wstring& distribution);
    static int ShowUsbHelp();

    // Parse USB-related command line arguments
    static bool ParseUsbCommand(_In_ int argc, _In_reads_(argc) wchar_t** argv, _Out_ int& exitCode);

private:
    struct UsbDeviceDisplay {
        std::wstring InstanceId;
        std::wstring Description;
        std::wstring VendorId;
        std::wstring ProductId;
        std::wstring Status;
        std::wstring AttachedTo;
    };

    static std::vector<UsbDeviceDisplay> EnumerateUsbDevicesForDisplay();
    static void PrintUsbDeviceList(_In_ const std::vector<UsbDeviceDisplay>& devices, _In_ bool verbose);
    static std::wstring GetDeviceInstanceIdFromFriendlyId(_In_ const std::wstring& friendlyId);
};

} // namespace wsl::windows::common
