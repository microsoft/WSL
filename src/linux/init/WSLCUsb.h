/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCUsb.h

Abstract:

    USB/IP client used to import USB devices shared by the Windows host into the
    WSLC virtual machine, so that containers can be given access to them.

--*/

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace wsl::linux::usb {

struct AttachResult
{
    // The devices that were imported. When "all" was requested this is the list
    // it resolved to, so the caller can release exactly what it took.
    std::vector<std::string> BusIds;

    // Class device nodes that appeared, such as "/dev/ttyUSB0". A device with no
    // class driver, such as one used through libusb, reports none; callers reach
    // those through /dev/bus/usb instead.
    std::vector<std::string> DeviceNodes;
};

// Imports a device from a USB/IP server. BusId may be "all" to import every
// device the server is currently sharing.
//
// Importing the same bus ID again takes a reference rather than importing twice,
// so several containers can be given the same device.
AttachResult Attach(const std::string& Host, uint16_t Port, const std::string& BusId);

// Drops a reference taken by Attach() and releases the device once the last one
// is gone, returning it to the host.
void Detach(const std::string& BusId);

} // namespace wsl::linux::usb
