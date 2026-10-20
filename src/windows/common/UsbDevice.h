// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#include "UsbTypes.h"
#include <windows.h>
#include <wil/resource.h>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wsl::windows::common::usb {

struct Hub
{
    std::wstring InstanceId;
    std::wstring InterfacePath;
    std::wstring ParentInstanceId;
    uint32_t Port = 0;
};

struct Device
{
    bool IsRedirected() const;
    bool IsReady() const;
    std::optional<std::wstring> ProbeWinUsb() const;

    std::wstring HubInstanceId;
    uint32_t Port = 0;
    std::wstring InstanceId;
    std::wstring Name;
    std::wstring DisplayInstanceId;
    std::vector<std::wstring> HardwareIds;
    std::vector<std::wstring> InterfacePaths;
    std::wstring Service;
    ULONG Status = 0;
    ULONG Problem = 0;
};

class Inventory
{
public:
    explicit Inventory(std::vector<Hub> Hubs);

    static Inventory Enumerate();
    std::vector<const Hub*> GetHubPath(std::wstring_view InstanceId) const;
    PortId GetPortId(const Device& Value) const;
    const Hub& ResolveParentHub(const PortId& Port) const;

    std::vector<Hub> Hubs;
    std::vector<Device> Devices;

private:
    static wil::srwlock s_rootHubsLock;
    _Guarded_by_(s_rootHubsLock) static std::vector<std::wstring> s_rootHubs;
    std::vector<std::wstring> m_rootHubs;
};

void GrantDeviceAccess(const std::wstring& InterfacePath, PSID UserSid);
void CyclePort(const std::wstring& HubInstanceId, uint32_t Port);

} // namespace wsl::windows::common::usb