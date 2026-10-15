// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"
#include "UsbDevice.h"
#include "UsbUxdRegistry.h"
#include "stringshared.h"
#include <cfgmgr32.h>
#include <setupapi.h>
#include <usbiodef.h>
#include <usbioctl.h>
#include <winusb.h>

namespace wsl::windows::common::usb {

using wsl::shared::string::IsEqual;

namespace {

    using DeviceInfoSet = wil::unique_any<HDEVINFO, decltype(&SetupDiDestroyDeviceInfoList), SetupDiDestroyDeviceInfoList>;
    using WinUsbHandle = wil::unique_any<WINUSB_INTERFACE_HANDLE, decltype(&WinUsb_Free), WinUsb_Free>;

    void ThrowIfConfigurationManagerError(CONFIGRET Result)
    {
        THROW_IF_WIN32_ERROR(CM_MapCrToWin32Err(Result, ERROR_GEN_FAILURE));
    }

    std::wstring GetDeviceInstanceId(DEVINST Node)
    {
        wchar_t value[MAX_DEVICE_ID_LEN]{};
        ThrowIfConfigurationManagerError(CM_Get_Device_IDW(Node, value, ARRAYSIZE(value), 0));
        return value;
    }

    std::vector<BYTE> GetDevicePropertyBytes(DEVINST Node, const DEVPROPKEY& Key, DEVPROPTYPE Expected)
    {
        // Query the required property buffer size.
        DEVPROPTYPE type{};
        ULONG size = 0;
        auto result = CM_Get_DevNode_PropertyW(Node, &Key, &type, nullptr, &size, 0);
        if (result == CR_NO_SUCH_VALUE)
        {
            return {};
        }
        if (result != CR_BUFFER_SMALL)
        {
            ThrowIfConfigurationManagerError(result);
        }

        // Read the property once and validate the returned type.
        std::vector<BYTE> bytes(size);
        result = CM_Get_DevNode_PropertyW(Node, &Key, &type, bytes.data(), &size, 0);
        if (result == CR_NO_SUCH_VALUE)
        {
            return {};
        }
        ThrowIfConfigurationManagerError(result);
        THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_DATA), type != Expected && !(Expected == DEVPROP_TYPE_UINT32 && type == DEVPROP_TYPE_INT32));
        bytes.resize(size);
        return bytes;
    }

    std::vector<std::wstring> GetDevicePropertyStrings(DEVINST Node, const DEVPROPKEY& Key, DEVPROPTYPE Type = DEVPROP_TYPE_STRING)
    {
        const auto bytes = GetDevicePropertyBytes(Node, Key, Type);
        if (bytes.empty())
        {
            return {};
        }

        THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_DATA), bytes.size() % sizeof(wchar_t) != 0);
        std::wstring text(bytes.size() / sizeof(wchar_t), L'\0');
        memcpy(text.data(), bytes.data(), bytes.size());
        THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_DATA), text.back() != L'\0');

        // Split the null-terminated values in string and string-list properties.
        std::vector<std::wstring> result;
        for (size_t offset = 0; offset < text.size() && text[offset] != L'\0';)
        {
            const auto end = text.find(L'\0', offset);
            result.push_back(text.substr(offset, end - offset));
            offset = end + 1;
        }
        return result;
    }

    std::wstring GetDevicePropertyString(DEVINST Node, const DEVPROPKEY& Key)
    {
        const auto values = GetDevicePropertyStrings(Node, Key);
        return values.empty() ? std::wstring{} : values.front();
    }

    std::vector<Hub> EnumerateDeviceInterfaces(const GUID& Class, PCWSTR Instance = nullptr)
    {
        const auto raw = SetupDiGetClassDevsW(&Class, Instance, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
        THROW_LAST_ERROR_IF(raw == INVALID_HANDLE_VALUE);
        DeviceInfoSet devices(raw);

        std::vector<Hub> result;
        for (DWORD index = 0;; ++index)
        {
            SP_DEVICE_INTERFACE_DATA entry{sizeof(entry)};
            if (!SetupDiEnumDeviceInterfaces(devices.get(), nullptr, &Class, index, &entry))
            {
                const auto error = GetLastError();
                if (error == ERROR_NO_MORE_ITEMS)
                {
                    return result;
                }
                THROW_WIN32(error);
            }

            // Query the detail size, then retrieve the interface path and device instance.
            DWORD size = 0;
            SetupDiGetDeviceInterfaceDetailW(devices.get(), &entry, nullptr, 0, &size, nullptr);
            const auto error = GetLastError();
            THROW_WIN32_IF(error, error != ERROR_INSUFFICIENT_BUFFER);

            std::vector<BYTE> buffer(size);
            auto* details = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(buffer.data());
            details->cbSize = sizeof(*details);
            SP_DEVINFO_DATA node{sizeof(node)};
            THROW_IF_WIN32_BOOL_FALSE(SetupDiGetDeviceInterfaceDetailW(devices.get(), &entry, details, size, nullptr, &node));

            result.push_back({GetDeviceInstanceId(node.DevInst), details->DevicePath});
        }
    }

} // namespace

Inventory Inventory::Enumerate()
{
    // Enumerate hubs first so physical devices can be identified by their parent.
    Inventory result;
    result.Hubs = EnumerateDeviceInterfaces(GUID_DEVINTERFACE_USB_HUB);

    const auto raw = SetupDiGetClassDevsW(nullptr, L"USB", nullptr, DIGCF_PRESENT | DIGCF_ALLCLASSES);
    THROW_LAST_ERROR_IF(raw == INVALID_HANDLE_VALUE);
    DeviceInfoSet devices(raw);

    for (DWORD index = 0;; ++index)
    {
        SP_DEVINFO_DATA node{sizeof(node)};
        if (!SetupDiEnumDeviceInfo(devices.get(), index, &node))
        {
            const auto error = GetLastError();
            if (error == ERROR_NO_MORE_ITEMS)
            {
                return result;
            }
            THROW_WIN32(error);
        }

        try
        {
            // Keep devices directly connected to a hub; exclude hubs and composite functions.
            DEVINST parent{};
            ThrowIfConfigurationManagerError(CM_Get_Parent(&parent, node.DevInst, 0));
            Device device;
            device.HubInstanceId = GetDeviceInstanceId(parent);
            device.InstanceId = GetDeviceInstanceId(node.DevInst);
            if (!std::ranges::any_of(
                    result.Hubs, [&](const auto& hub) { return IsEqual(hub.InstanceId, device.HubInstanceId, true); }) ||
                std::ranges::any_of(result.Hubs, [&](const auto& hub) { return IsEqual(hub.InstanceId, device.InstanceId, true); }))
            {
                continue;
            }

            const auto compatible = GetDevicePropertyStrings(node.DevInst, DEVPKEY_Device_CompatibleIds, DEVPROP_TYPE_STRING_LIST);
            if (std::ranges::any_of(
                    compatible, [](const auto& id) { return wsl::shared::string::StartsWith(id, L"USB\\Class_09", true); }))
            {
                continue;
            }

            // Read the device's physical port number on its parent hub.
            const auto address = GetDevicePropertyBytes(node.DevInst, DEVPKEY_Device_Address, DEVPROP_TYPE_UINT32);
            if (address.size() != sizeof(device.Port))
            {
                continue;
            }
            memcpy(&device.Port, address.data(), sizeof(device.Port));
            if (device.Port == 0 || device.Port > 999)
            {
                continue;
            }

            // Prefer the friendly name, then fall back to bus and device descriptions.
            device.Name = GetDevicePropertyString(node.DevInst, DEVPKEY_Device_FriendlyName);
            if (device.Name.empty())
            {
                device.Name = GetDevicePropertyString(node.DevInst, DEVPKEY_Device_BusReportedDeviceDesc);
            }
            if (device.Name.empty())
            {
                device.Name = GetDevicePropertyString(node.DevInst, DEVPKEY_Device_DeviceDesc);
            }

            // Collect display IDs and the interface state needed to detect redirection.
            device.DisplayInstanceId = device.InstanceId;
            std::ranges::replace(device.DisplayInstanceId, L'\\', L'#');
            device.HardwareIds = GetDevicePropertyStrings(node.DevInst, DEVPKEY_Device_HardwareIds, DEVPROP_TYPE_STRING_LIST);
            device.Service = GetDevicePropertyString(node.DevInst, DEVPKEY_Device_Service);
            ThrowIfConfigurationManagerError(CM_Get_DevNode_Status(&device.Status, &device.Problem, node.DevInst, 0));
            for (auto& entry : EnumerateDeviceInterfaces(GUID_DEVINTERFACE_USB_DEVICE, device.InstanceId.c_str()))
            {
                device.InterfacePaths.push_back(std::move(entry.InterfacePath));
            }

            result.Devices.push_back(std::move(device));
        }
        CATCH_LOG()
    }
}

bool Device::IsRedirected() const
{
    return std::ranges::any_of(HardwareIds, [](const auto& id) { return IsEqual(id, c_winusbHardwareId, true); });
}

bool Device::IsReady() const
{
    return IsRedirected() && IsEqual(Service, L"WINUSB", true) && WI_IsFlagSet(Status, DN_STARTED) &&
           WI_IsFlagClear(Status, DN_HAS_PROBLEM) && !InterfacePaths.empty();
}

std::optional<std::wstring> Device::ProbeWinUsb() const
{
    if (!IsReady())
    {
        return {};
    }

    // Confirm the redirected interface can be opened and initialized by WinUSB.
    const auto& path = InterfacePaths.front();
    wil::unique_hfile file(CreateFileW(
        path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr));
    THROW_LAST_ERROR_IF_MSG(!file, "Opening USB device '%ls'", path.c_str());

    WinUsbHandle usb;
    THROW_IF_WIN32_BOOL_FALSE_MSG(WinUsb_Initialize(file.get(), usb.put()), "Initializing WinUSB device '%ls'", path.c_str());
    return path;
}

void GrantDeviceAccess(const std::wstring& InterfacePath, PSID UserSid)
{
    THROW_HR_IF(E_INVALIDARG, !IsValidSid(UserSid));

    wil::unique_hfile file(CreateFileW(
        InterfacePath.c_str(), READ_CONTROL | WRITE_DAC, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr));
    THROW_LAST_ERROR_IF(!file);

    // Grant full access to SYSTEM and administrators, and read/write access to the owning user.
    BYTE systemSid[SECURITY_MAX_SID_SIZE]{};
    BYTE adminSid[SECURITY_MAX_SID_SIZE]{};
    DWORD size = sizeof(systemSid);
    THROW_IF_WIN32_BOOL_FALSE(CreateWellKnownSid(WinLocalSystemSid, nullptr, systemSid, &size));
    size = sizeof(adminSid);
    THROW_IF_WIN32_BOOL_FALSE(CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr, adminSid, &size));

    EXPLICIT_ACCESSW access[3]{};
    const PSID trustees[] = {systemSid, adminSid, UserSid};
    for (size_t index = 0; index < ARRAYSIZE(access); ++index)
    {
        access[index].grfAccessPermissions = index == 2 ? GENERIC_READ | GENERIC_WRITE : GENERIC_ALL;
        access[index].grfAccessMode = SET_ACCESS;
        access[index].grfInheritance = NO_INHERITANCE;
        BuildTrusteeWithSidW(&access[index].Trustee, trustees[index]);
    }

    wil::unique_hlocal_ptr<ACL> acl;
    THROW_IF_WIN32_ERROR(SetEntriesInAclW(ARRAYSIZE(access), access, nullptr, wil::out_param(acl)));
    THROW_IF_WIN32_ERROR(SetSecurityInfo(file.get(), SE_KERNEL_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, acl.get(), nullptr));
}

void CyclePort(const std::wstring& HubInstanceId, uint32_t Port)
{
    const auto hubs = EnumerateDeviceInterfaces(GUID_DEVINTERFACE_USB_HUB, HubInstanceId.c_str());
    THROW_WIN32_IF(ERROR_NOT_FOUND, hubs.empty());
    wil::unique_hfile file(CreateFileW(
        hubs.front().InterfacePath.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr));
    THROW_LAST_ERROR_IF(!file);

    // Cycle the selected physical port through the hub driver.
    USB_CYCLE_PORT_PARAMS parameters{};
    parameters.ConnectionIndex = Port;
    DWORD returned = 0;
    THROW_IF_WIN32_BOOL_FALSE(DeviceIoControl(
        file.get(), IOCTL_USB_HUB_CYCLE_PORT, &parameters, sizeof(parameters), &parameters, sizeof(parameters), &returned, nullptr));

    THROW_HR_IF_MSG(
        HRESULT_FROM_WIN32(ERROR_GEN_FAILURE),
        parameters.StatusReturned != 0,
        "Cycling USB port failed with USB status 0x%08X",
        parameters.StatusReturned);
}

} // namespace wsl::windows::common::usb