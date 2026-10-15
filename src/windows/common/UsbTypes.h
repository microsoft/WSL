// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace wsl::windows::common::usb {

enum class DeviceState
{
    Unattached,
    Attached,
    Unavailable
};

struct PortSelector
{
    uint32_t Hub;
    uint32_t Port;
};

struct ListedDevice
{
    std::wstring Port;
    std::wstring Name;
    std::wstring HardwareId;
    DeviceState State;
};

PortSelector ParsePortSelector(std::wstring_view Value);

} // namespace wsl::windows::common::usb