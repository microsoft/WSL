/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    usbclient.cpp

Abstract:

    This file contains USB command-line interface implementation for wsl.exe.
    Provides commands for USB device management.

--*/

#include "precomp.h"
#include "usbclient.hpp"
#include "usbservice.hpp"
#include "hvsocket.hpp"
#include <iostream>
#include <iomanip>
#include <algorithm>

namespace wsl::windows::common {

// Parse USB command line arguments
bool UsbClient::ParseUsbCommand(_In_ int argc, _In_reads_(argc) wchar_t** argv, _Out_ int& exitCode)
{
    exitCode = 0;

    for (int i = 1; i < argc; i++)
    {
        std::wstring arg = argv[i];
        std::transform(arg.begin(), arg.end(), arg.begin(), ::towlower);

        if (arg == L"--usb-list" || arg == L"--usb-list-devices")
        {
            bool verbose = false;
            // Check for --verbose flag
            if (i + 1 < argc)
            {
                std::wstring nextArg = argv[i + 1];
                std::transform(nextArg.begin(), nextArg.end(), nextArg.begin(), ::towlower);
                if (nextArg == L"--verbose" || nextArg == L"-v")
                {
                    verbose = true;
                    i++;
                }
            }
            exitCode = ListUsbDevices(verbose);
            return true;
        }
        else if (arg == L"--usb-attach")
        {
            if (i + 1 >= argc)
            {
                std::wcerr << L"Error: --usb-attach requires a device ID" << std::endl;
                exitCode = 1;
                return true;
            }

            std::wstring deviceId = argv[++i];
            std::wstring distribution;

            // Check for optional --distribution flag
            if (i + 1 < argc)
            {
                std::wstring nextArg = argv[i + 1];
                std::transform(nextArg.begin(), nextArg.end(), nextArg.begin(), ::towlower);
                if (nextArg == L"--distribution" || nextArg == L"-d")
                {
                    if (i + 2 >= argc)
                    {
                        std::wcerr << L"Error: --distribution requires a distribution name" << std::endl;
                        exitCode = 1;
                        return true;
                    }
                    distribution = argv[i + 2];
                    i += 2;
                }
            }

            exitCode = AttachUsbDevice(deviceId, distribution);
            return true;
        }
        else if (arg == L"--usb-detach")
        {
            if (i + 1 >= argc)
            {
                std::wcerr << L"Error: --usb-detach requires a device ID" << std::endl;
                exitCode = 1;
                return true;
            }

            std::wstring deviceId = argv[++i];
            std::wstring distribution;

            // Check for optional --distribution flag
            if (i + 1 < argc)
            {
                std::wstring nextArg = argv[i + 1];
                std::transform(nextArg.begin(), nextArg.end(), nextArg.begin(), ::towlower);
                if (nextArg == L"--distribution" || nextArg == L"-d")
                {
                    if (i + 2 >= argc)
                    {
                        std::wcerr << L"Error: --distribution requires a distribution name" << std::endl;
                        exitCode = 1;
                        return true;
                    }
                    distribution = argv[i + 2];
                    i += 2;
                }
            }

            exitCode = DetachUsbDevice(deviceId, distribution);
            return true;
        }
        else if (arg == L"--usb-help")
        {
            exitCode = ShowUsbHelp();
            return true;
        }
    }

    return false;
}

// List USB devices
int UsbClient::ListUsbDevices(_In_ bool verbose)
{
    try
    {
        auto devices = EnumerateUsbDevicesForDisplay();

        if (devices.empty())
        {
            std::wcout << L"No USB devices found." << std::endl;
            return 0;
        }

        PrintUsbDeviceList(devices, verbose);
        return 0;
    }
    catch (const std::exception& e)
    {
        std::cerr << "Error enumerating USB devices: " << e.what() << std::endl;
        return 1;
    }
}

// Attach USB device
int UsbClient::AttachUsbDevice(_In_ const std::wstring& deviceId, _In_opt_ const std::wstring& distribution)
{
    try
    {
        // Get full instance ID if abbreviated ID was provided
        std::wstring instanceId = GetDeviceInstanceIdFromFriendlyId(deviceId);
        if (instanceId.empty())
        {
            std::wcerr << L"Error: Device not found: " << deviceId << std::endl;
            return 1;
        }

        // Initialize USB service
        usb::UsbService usbService;
        RETURN_IF_FAILED_MSG(usbService.Initialize(), "Failed to initialize USB service");

        // Get the distribution's VM ID (if not specified, use default)
        GUID vmId = {}; // This would be retrieved from the distribution
        
        // Connect to the distribution's USB service
        auto hvSocket = hvsocket::Connect(vmId, usb::USB_PASSTHROUGH_PORT);
        RETURN_HR_IF(E_FAIL, !hvSocket);

        // Convert instance ID to narrow string
        int narrowSize = WideCharToMultiByte(CP_UTF8, 0, instanceId.c_str(), -1, nullptr, 0, nullptr, nullptr);
        std::string narrowInstanceId(narrowSize, 0);
        WideCharToMultiByte(CP_UTF8, 0, instanceId.c_str(), -1, &narrowInstanceId[0], narrowSize, nullptr, nullptr);
        narrowInstanceId.resize(narrowSize - 1); // Remove null terminator

        // Attach the device
        HRESULT hr = usbService.AttachDevice(narrowInstanceId, hvSocket.get());
        if (FAILED(hr))
        {
            std::wcerr << L"Error: Failed to attach device. Make sure the device is not already attached." << std::endl;
            return 1;
        }

        std::wcout << L"Successfully attached device: " << instanceId << std::endl;
        if (!distribution.empty())
        {
            std::wcout << L"To distribution: " << distribution << std::endl;
        }

        return 0;
    }
    catch (const std::exception& e)
    {
        std::cerr << "Error attaching USB device: " << e.what() << std::endl;
        return 1;
    }
}

// Detach USB device
int UsbClient::DetachUsbDevice(_In_ const std::wstring& deviceId, _In_opt_ const std::wstring& distribution)
{
    try
    {
        // Get full instance ID if abbreviated ID was provided
        std::wstring instanceId = GetDeviceInstanceIdFromFriendlyId(deviceId);
        if (instanceId.empty())
        {
            std::wcerr << L"Error: Device not found: " << deviceId << std::endl;
            return 1;
        }

        // Initialize USB service
        usb::UsbService usbService;
        RETURN_IF_FAILED_MSG(usbService.Initialize(), "Failed to initialize USB service");

        // Convert instance ID to narrow string
        int narrowSize = WideCharToMultiByte(CP_UTF8, 0, instanceId.c_str(), -1, nullptr, 0, nullptr, nullptr);
        std::string narrowInstanceId(narrowSize, 0);
        WideCharToMultiByte(CP_UTF8, 0, instanceId.c_str(), -1, &narrowInstanceId[0], narrowSize, nullptr, nullptr);
        narrowInstanceId.resize(narrowSize - 1);

        // Detach the device
        HRESULT hr = usbService.DetachDevice(narrowInstanceId);
        if (FAILED(hr))
        {
            std::wcerr << L"Error: Failed to detach device. Make sure the device is currently attached." << std::endl;
            return 1;
        }

        std::wcout << L"Successfully detached device: " << instanceId << std::endl;
        return 0;
    }
    catch (const std::exception& e)
    {
        std::cerr << "Error detaching USB device: " << e.what() << std::endl;
        return 1;
    }
}

// Show USB help
int UsbClient::ShowUsbHelp()
{
    std::wcout << L"\nWSL USB Device Management Commands:\n\n";
    std::wcout << L"  wsl --usb-list [--verbose]\n";
    std::wcout << L"      List all available USB devices on the host.\n";
    std::wcout << L"      Use --verbose for detailed information.\n\n";
    std::wcout << L"  wsl --usb-attach <device-id> [--distribution <name>]\n";
    std::wcout << L"      Attach a USB device to WSL.\n";
    std::wcout << L"      device-id: Device instance ID or busid (e.g., 'USB\\VID_1234&PID_5678\\...' or '1-1')\n";
    std::wcout << L"      --distribution: Optional. Attach to a specific distribution (default: default distribution)\n\n";
    std::wcout << L"  wsl --usb-detach <device-id> [--distribution <name>]\n";
    std::wcout << L"      Detach a USB device from WSL.\n";
    std::wcout << L"      device-id: Device instance ID or busid used during attach\n\n";
    std::wcout << L"Examples:\n";
    std::wcout << L"  wsl --usb-list\n";
    std::wcout << L"  wsl --usb-attach USB\\VID_1234&PID_5678\\6&1234ABCD\n";
    std::wcout << L"  wsl --usb-attach 1-1 --distribution Ubuntu\n";
    std::wcout << L"  wsl --usb-detach 1-1\n\n";
    std::wcout << L"Note: This feature uses Hyper-V sockets and does not require IP networking.\n";
    std::wcout << L"      It works reliably with VPNs and complex network configurations.\n";
    
    return 0;
}

// Enumerate USB devices for display
std::vector<UsbClient::UsbDeviceDisplay> UsbClient::EnumerateUsbDevicesForDisplay()
{
    std::vector<UsbDeviceDisplay> displayDevices;

    usb::UsbService usbService;
    if (FAILED(usbService.Initialize()))
    {
        return displayDevices;
    }

    auto devices = usbService.EnumerateDevices();

    for (const auto& device : devices)
    {
        UsbDeviceDisplay display;
        
        // Convert narrow strings to wide
        int wideSize = MultiByteToWideChar(CP_UTF8, 0, device.InstanceId, -1, nullptr, 0);
        std::wstring wideInstanceId(wideSize, 0);
        MultiByteToWideChar(CP_UTF8, 0, device.InstanceId, -1, &wideInstanceId[0], wideSize);
        wideInstanceId.resize(wideSize - 1);
        display.InstanceId = wideInstanceId;

        wideSize = MultiByteToWideChar(CP_UTF8, 0, device.DeviceDesc, -1, nullptr, 0);
        std::wstring wideDesc(wideSize, 0);
        MultiByteToWideChar(CP_UTF8, 0, device.DeviceDesc, -1, &wideDesc[0], wideSize);
        wideDesc.resize(wideSize - 1);
        display.Description = wideDesc;

        // Format VID/PID
        wchar_t vidpid[32];
        swprintf_s(vidpid, L"%04X:%04X", device.VendorId, device.ProductId);
        display.VendorId = std::to_wstring(device.VendorId);
        display.ProductId = std::to_wstring(device.ProductId);

        // Status
        display.Status = device.IsAttached ? L"Attached" : L"Available";
        display.AttachedTo = device.IsAttached ? L"WSL" : L"";

        displayDevices.push_back(display);
    }

    return displayDevices;
}

// Print USB device list
void UsbClient::PrintUsbDeviceList(_In_ const std::vector<UsbDeviceDisplay>& devices, _In_ bool verbose)
{
    std::wcout << L"\nUSB Devices:\n";
    std::wcout << L"============\n\n";

    for (const auto& device : devices)
    {
        std::wcout << L"Device: " << device.Description << std::endl;
        std::wcout << L"  VID:PID: " << device.VendorId << L":" << device.ProductId << std::endl;
        std::wcout << L"  Status: " << device.Status;
        if (!device.AttachedTo.empty())
        {
            std::wcout << L" (to " << device.AttachedTo << L")";
        }
        std::wcout << std::endl;

        if (verbose)
        {
            std::wcout << L"  Instance ID: " << device.InstanceId << std::endl;
        }

        std::wcout << std::endl;
    }

    std::wcout << L"Total devices: " << devices.size() << std::endl;
}

// Get device instance ID from friendly ID (e.g., busid or abbreviated ID)
std::wstring UsbClient::GetDeviceInstanceIdFromFriendlyId(_In_ const std::wstring& friendlyId)
{
    // If it already looks like a full instance ID, return it
    if (friendlyId.find(L"USB\\") != std::wstring::npos)
    {
        return friendlyId;
    }

    // Otherwise, search for a device matching the friendly ID
    auto devices = EnumerateUsbDevicesForDisplay();

    for (const auto& device : devices)
    {
        // Check if the friendly ID matches any part of the instance ID
        if (device.InstanceId.find(friendlyId) != std::wstring::npos)
        {
            return device.InstanceId;
        }

        // Check if it matches VID:PID format
        std::wstring vidpid = device.VendorId + L":" + device.ProductId;
        if (vidpid == friendlyId)
        {
            return device.InstanceId;
        }
    }

    // If no match found, return the original (it might be a valid busid format)
    return friendlyId;
}

} // namespace wsl::windows::common
