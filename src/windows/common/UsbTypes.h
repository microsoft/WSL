// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#include <guiddef.h>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace wsl::windows::common::usb {

enum class DeviceState
{
    Unattached,
    Attached,
    Unavailable
};

struct PortId
{
    std::vector<uint32_t> Parts;

    PortId() = default;
    explicit PortId(std::wstring_view Value);

    std::wstring ToString() const;
    bool operator==(const PortId&) const = default;
};

struct ListedDevice
{
    std::wstring Port;
    std::wstring Name;
    std::wstring HardwareId;
    DeviceState State;
};

struct DeviceReservation
{
    GUID Id;
    PortId Port;
    std::wstring DeviceInstanceId;
};

} // namespace wsl::windows::common::usb