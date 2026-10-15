// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#include <windows.h>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace wsl::windows::common::usb {

struct Hub
{
    std::wstring InstanceId;
    std::wstring InterfacePath;
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

struct Inventory
{
    static Inventory Enumerate();

    std::vector<Hub> Hubs;
    std::vector<Device> Devices;
};

void GrantDeviceAccess(const std::wstring& InterfacePath, PSID UserSid);
void CyclePort(const std::wstring& HubInstanceId, uint32_t Port);

} // namespace wsl::windows::common::usb